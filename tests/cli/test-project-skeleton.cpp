#include <project-skeleton.hpp>

#include <core/error/error.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

// The directories an authoring session writes into, laid out for a project that
// does not have them.
//
// WHY THE CASES ARE ABOUT ABSENCE RATHER THAN CREATION. Creating a directory is
// not the interesting half -- `create_directories` does that. What this function
// is FOR is that `task::ProjectFileStore` refuses a name whose parent is not
// there, deliberately, and nothing else in the product may create one: the store
// proves confinement by canonicalizing a parent that really exists, and the
// script layer above it has no directory verb at all. So the cases below pin the
// three directories by name, that a second run changes nothing, and that a file
// sitting where a directory belongs is reported rather than worked around.
namespace uf::cli
{
    namespace
    {
        class TemporaryDirectory final
        {
            std::filesystem::path m_path;

        public:
            explicit TemporaryDirectory(std::string_view label)
                : m_path{std::filesystem::temp_directory_path() / label}
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
                REQUIRE(std::filesystem::create_directories(m_path, error));
            }

            TemporaryDirectory(TemporaryDirectory const&)                    = delete;
            TemporaryDirectory(TemporaryDirectory&&)                         = delete;
            auto operator=(TemporaryDirectory const&) -> TemporaryDirectory& = delete;
            auto operator=(TemporaryDirectory&&) -> TemporaryDirectory&      = delete;

            ~TemporaryDirectory()
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
            }

            [[nodiscard]] auto path() const noexcept -> std::filesystem::path const&
            {
                return m_path;
            }
        };

        // Empty for a status that succeeded. doctest evaluates a *_MESSAGE
        // argument whether or not the assertion holds, so reaching into
        // `error()` here would trip on every passing case.
        [[nodiscard]]
        auto messageOf(Status const& status) -> std::string
        {
            if (status.has_value())
            {
                return std::string{};
            }
            return std::string{status.error().message()};
        }
    }

    TEST_CASE("a project with no skeleton gets the three directories it is authored into")
    {
        auto const directory = TemporaryDirectory{"uf-skeleton-fresh"};

        auto const laid = ensureProjectSkeleton(directory.path());
        REQUIRE_MESSAGE(laid.has_value(), messageOf(laid));

        // The three the product spells elsewhere: a template's store, a screen's
        // store, and the captures a session worked from.
        CHECK(std::filesystem::is_directory(directory.path() / "assets" / "templates"));
        CHECK(std::filesystem::is_directory(directory.path() / "assets" / "screens"));
        CHECK(std::filesystem::is_directory(directory.path() / "frames"));

        // Nothing else. A skeleton that invented a page model would be a model
        // nobody wrote, and the geometry it would have to state is a fact about
        // the target rather than about the directory.
        CHECK_FALSE(std::filesystem::exists(directory.path() / "page-model.toml"));
    }

    TEST_CASE("laying out a skeleton twice changes nothing the first run made")
    {
        auto const directory = TemporaryDirectory{"uf-skeleton-idempotent"};

        auto const first = ensureProjectSkeleton(directory.path());
        REQUIRE_MESSAGE(first.has_value(), messageOf(first));

        // A file already in one of them is what a second run would destroy if it
        // recreated rather than checked, and losing an authored template asset
        // that way would be silent.
        auto const asset = directory.path() / "assets" / "templates" / "kept.png";
        {
            auto stream = std::ofstream{asset, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            stream << "pixels";
        }

        auto const second = ensureProjectSkeleton(directory.path());
        REQUIRE_MESSAGE(second.has_value(), messageOf(second));
        CHECK(std::filesystem::is_regular_file(asset));
    }

    TEST_CASE("a file where a skeleton directory belongs is reported and not replaced")
    {
        auto const directory = TemporaryDirectory{"uf-skeleton-occupied"};

        auto const occupied = directory.path() / "frames";
        {
            auto stream = std::ofstream{occupied, std::ios::binary | std::ios::trunc};
            REQUIRE(stream.is_open());
            stream << "not a directory";
        }

        auto const laid = ensureProjectSkeleton(directory.path());
        REQUIRE_FALSE(laid.has_value());
        CHECK(messageOf(laid).find("frames") != std::string::npos);
        CHECK(
            messageOf(laid).find("is not a directory") != std::string::npos
        );
        CHECK(std::filesystem::is_regular_file(occupied));
    }

    TEST_CASE("a project directory that does not exist is refused rather than created")
    {
        auto const directory = TemporaryDirectory{"uf-skeleton-absent"};
        auto const absent    = directory.path() / "no-such-project";

        auto const laid = ensureProjectSkeleton(absent);
        REQUIRE_FALSE(laid.has_value());
        CHECK_FALSE(std::filesystem::exists(absent));
    }
}
