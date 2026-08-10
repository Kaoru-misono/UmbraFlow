#include <task/platform/confined-file.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::task_platform
{
    namespace
    {
        class TemporaryDirectory final
        {
            std::filesystem::path m_path{};

        public:
            TemporaryDirectory()
            {
                static auto s_sequence = std::atomic<unsigned long long>{1};
                m_path = std::filesystem::temp_directory_path()
                    / std::format(
                        "umbraflow-confined-{}-{}",
                        std::chrono::steady_clock::now().time_since_epoch().count(),
                        s_sequence.fetch_add(1, std::memory_order_relaxed)
                    );
                REQUIRE(std::filesystem::create_directories(m_path));
            }

            TemporaryDirectory(TemporaryDirectory const&) = delete;
            TemporaryDirectory(TemporaryDirectory&&) = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory& = delete;

            ~TemporaryDirectory() noexcept
            {
                auto error = std::error_code{};
                static_cast<void>(std::filesystem::remove_all(m_path, error));
            }

            [[nodiscard]] auto path() const -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        auto write(std::filesystem::path const& path, std::string_view bytes) -> void
        {
            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.good());
            stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(stream.good());
        }

        // A directory link that needs no privilege on Windows and that lstat
        // reports as a plain directory, which is what makes it the case worth
        // testing rather than a symlink.
        [[nodiscard]]
        auto linkDirectory(
            std::filesystem::path const& link,
            std::filesystem::path const& target
        ) -> bool
        {
#if defined(_WIN32)
            auto const command = std::format(
                "cmd /c mklink /J \"{}\" \"{}\" >nul 2>&1",
                link.string(),
                target.string()
            );
            return std::system(command.c_str()) == 0;
#else
            auto error = std::error_code{};
            std::filesystem::create_directory_symlink(target, link, error);
            return !error;
#endif
        }

        auto createChain(
            std::filesystem::path const& base,
            std::size_t levels
        ) -> void
        {
            auto path = base;
            for (auto level = std::size_t{0}; level < levels; ++level)
            {
                path /= "d";
            }
            REQUIRE(std::filesystem::create_directories(path));
        }
    }

    TEST_CASE("a confined root reads exactly the bytes a declared path holds")
    {
        auto const temporary = TemporaryDirectory{};
        std::filesystem::create_directories(temporary.path() / "assets");
        write(temporary.path() / "manifest.json", "{}");
        write(temporary.path() / "assets" / "one.png", "pixels");

        auto root = ConfinedRoot::open(temporary.path());
        REQUIRE(root.has_value());

        auto const manifest = root->readFile("manifest.json", 1024U);
        REQUIRE(manifest.has_value());
        CHECK(manifest->size() == 2U);

        auto const nested = root->readFile("assets/one.png", 1024U);
        REQUIRE(nested.has_value());
        CHECK(nested->size() == 6U);
    }

    TEST_CASE("a confined root refuses a directory link on the way to a file")
    {
        auto const temporary = TemporaryDirectory{};
        auto const outside = temporary.path() / "outside";
        std::filesystem::create_directories(outside);
        write(outside / "one.png", "elsewhere");

        auto const inside = temporary.path() / "root";
        std::filesystem::create_directories(inside);
        write(inside / "manifest.json", "{}");
        auto const linked = linkDirectory(inside / "assets", outside);
#if defined(_WIN32)
        // A junction needs no privilege here, so a failure is a broken test
        // rather than an unavailable feature. Returning quietly would delete
        // the only reparse coverage in the repository without a signal.
        REQUIRE(linked);
#else
        if (!linked)
        {
            MESSAGE("this account cannot create a directory symlink");
            return;
        }
#endif

        // The link resolves for anything that walks the path by name, so this
        // is exactly the read that hash verification alone would not prevent
        // from reaching a file outside the artifact.
        auto root = ConfinedRoot::open(inside);
        REQUIRE(root.has_value());
        CHECK_FALSE(root->readFile("assets/one.png", 1024U).has_value());
    }

    TEST_CASE("a confined root refuses to open a linked root at all")
    {
        auto const temporary = TemporaryDirectory{};
        auto const outside = temporary.path() / "outside";
        std::filesystem::create_directories(outside);
        write(outside / "manifest.json", "{}");
        auto const linked = linkDirectory(temporary.path() / "root", outside);
#if defined(_WIN32)
        REQUIRE(linked);
#else
        if (!linked)
        {
            MESSAGE("this account cannot create a directory symlink");
            return;
        }
#endif
        CHECK_FALSE(ConfinedRoot::open(temporary.path() / "root").has_value());
    }

    TEST_CASE("a confined root refuses a path that tries to leave it")
    {
        auto const temporary = TemporaryDirectory{};
        write(temporary.path() / "manifest.json", "{}");
        auto root = ConfinedRoot::open(temporary.path());
        REQUIRE(root.has_value());

        CHECK_FALSE(root->readFile("../manifest.json", 1024U).has_value());
        CHECK_FALSE(root->readFile("assets//one.png", 1024U).has_value());
        CHECK_FALSE(root->readFile("./manifest.json", 1024U).has_value());
        CHECK_FALSE(root->readFile("", 1024U).has_value());

        // A ceiling below the file's size is refused rather than truncated.
        CHECK_FALSE(root->readFile("manifest.json", 1U).has_value());
    }

    TEST_CASE("a confined root lists its children and removes one tree whole")
    {
        auto const temporary = TemporaryDirectory{};
        std::filesystem::create_directories(temporary.path() / "orphan" / "assets");
        write(temporary.path() / "orphan" / "assets" / "one.png", "pixels");
        write(temporary.path() / "orphan" / "manifest.json", "{}");
        write(temporary.path() / "keep.txt", "kept");

        auto root = ConfinedRoot::open(temporary.path());
        REQUIRE(root.has_value());

        auto const names = root->childNames();
        REQUIRE(names.has_value());
        CHECK(names->size() == 2U);
        CHECK(std::ranges::contains(*names, std::string{"orphan"}));
        CHECK(std::ranges::contains(*names, std::string{"keep.txt"}));

        REQUIRE(root->removeTree("orphan").has_value());
        CHECK_FALSE(std::filesystem::exists(temporary.path() / "orphan"));
        CHECK(std::filesystem::exists(temporary.path() / "keep.txt"));

        // Removing what is already gone is the outcome the caller asked for.
        CHECK(root->removeTree("orphan").has_value());
    }

    TEST_CASE("a confined root refuses to remove a tree with a link planted in it")
    {
        auto const temporary = TemporaryDirectory{};
        auto const outside = temporary.path() / "outside";
        std::filesystem::create_directories(outside);
        write(outside / "canary.txt", "must survive");

        auto const inside = temporary.path() / "root";
        std::filesystem::create_directories(inside / "orphan");
        auto const nested = linkDirectory(inside / "orphan" / "assets", outside);
#if defined(_WIN32)
        REQUIRE(nested);
#else
        if (!nested)
        {
            MESSAGE("this account cannot create a directory symlink");
            return;
        }
#endif
        REQUIRE(linkDirectory(inside / "direct", outside));

        auto root = ConfinedRoot::open(inside);
        REQUIRE(root.has_value());

        // The link is refused rather than unlinked through, whether it is the
        // named child or something the walk reaches under it.
        CHECK_FALSE(root->removeTree("direct").has_value());
        CHECK_FALSE(root->removeTree("orphan").has_value());
        CHECK(std::filesystem::is_regular_file(outside / "canary.txt"));
    }

    TEST_CASE("a confined root refuses a removal deeper than its recursion ceiling")
    {
        auto const temporary = TemporaryDirectory{};
        auto root = ConfinedRoot::open(temporary.path());
        REQUIRE(root.has_value());

        // The positive control: nesting on its own is not what is refused.
        createChain(temporary.path() / "shallow", 8U);
        REQUIRE(root->removeTree("shallow").has_value());
        CHECK_FALSE(std::filesystem::exists(temporary.path() / "shallow"));

        createChain(temporary.path() / "deep", 40U);
        CHECK_FALSE(root->removeTree("deep").has_value());

        // Nothing is unlinked until every child call has returned, so the
        // refusal leaves the tree standing rather than half removed.
        CHECK(std::filesystem::exists(temporary.path() / "deep"));
    }

    TEST_CASE("a confined root refuses a removal name that is not a single child")
    {
        // The root sits one level down, so a name that escaped it could still
        // only reach this test's own directory.
        auto const temporary = TemporaryDirectory{};
        auto const inside = temporary.path() / "root";
        std::filesystem::create_directories(inside / "orphan" / "assets");

        auto root = ConfinedRoot::open(inside);
        REQUIRE(root.has_value());

        CHECK_FALSE(root->removeTree("").has_value());
        CHECK_FALSE(root->removeTree(".").has_value());
        CHECK_FALSE(root->removeTree("..").has_value());
        CHECK_FALSE(root->removeTree("orphan/assets").has_value());
        CHECK_FALSE(root->removeTree("orphan\\assets").has_value());
        CHECK(std::filesystem::exists(inside / "orphan" / "assets"));
    }

    TEST_CASE("a confined root stays bound to the directory it opened, not to its name")
    {
        // The two platforms hold this by different means and can therefore be
        // asked different questions, so both are asked rather than one skipped.
        // A Windows handle is opened without delete sharing, so the rename below
        // is refused and the substitution can never be staged; a POSIX
        // descriptor does not block the rename, so the substitution is staged
        // and every operation is then required to reach the opened directory
        // rather than the name it used to have.
        auto const temporary = TemporaryDirectory{};
        auto const inside = temporary.path() / "root";
        std::filesystem::create_directories(inside / "orphan");
        write(inside / "orphan" / "kept.txt", "opened");
        write(inside / "real.txt", "opened");

        auto root = ConfinedRoot::open(inside);
        REQUIRE(root.has_value());

        auto const moved = temporary.path() / "moved";
        auto renameError = std::error_code{};
        std::filesystem::rename(inside, moved, renameError);

#if defined(_WIN32)
        CHECK(static_cast<bool>(renameError));
        CHECK(std::filesystem::exists(inside / "real.txt"));
#else
        REQUIRE_FALSE(static_cast<bool>(renameError));

        // A decoy takes the vacated name. Its entries are named differently
        // from the opened tree's, so any operation that resolves the root by
        // name again is distinguishable from one that goes through the
        // descriptor.
        std::filesystem::create_directories(inside / "orphan");
        write(inside / "orphan" / "decoy.txt", "decoy");
        write(inside / "decoy.txt", "decoy");

        auto const names = root->childNames();
        REQUIRE(names.has_value());
        CHECK(std::ranges::contains(*names, std::string{"real.txt"}));
        CHECK_FALSE(std::ranges::contains(*names, std::string{"decoy.txt"}));

        CHECK(root->readFile("real.txt", 1024U).has_value());
        CHECK_FALSE(root->readFile("decoy.txt", 1024U).has_value());
        CHECK(root->readFile("orphan/kept.txt", 1024U).has_value());
        CHECK_FALSE(root->readFile("orphan/decoy.txt", 1024U).has_value());

        // The removal takes the opened directory's child; the decoy standing at
        // the old name keeps its own.
        REQUIRE(root->removeTree("orphan").has_value());
        CHECK_FALSE(std::filesystem::exists(moved / "orphan"));
        CHECK(std::filesystem::is_regular_file(inside / "orphan" / "decoy.txt"));
#endif
    }
}
