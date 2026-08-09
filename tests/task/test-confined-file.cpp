#include <task/platform/confined-file.hpp>

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
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
}
