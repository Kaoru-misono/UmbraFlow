#include <project/project-kit.hpp>

#include <core/error/error.hpp>

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::project
{
    namespace
    {
        class TemporaryWorkspace final
        {
            std::filesystem::path m_path;

        public:
            explicit TemporaryWorkspace(std::string_view label)
                : m_path{std::filesystem::temp_directory_path() / label}
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
                REQUIRE(std::filesystem::create_directories(source(), error));
            }

            TemporaryWorkspace(TemporaryWorkspace const&)                    = delete;
            TemporaryWorkspace(TemporaryWorkspace&&)                         = delete;
            auto operator=(TemporaryWorkspace const&) -> TemporaryWorkspace& = delete;
            auto operator=(TemporaryWorkspace&&) -> TemporaryWorkspace&      = delete;

            ~TemporaryWorkspace()
            {
                auto error = std::error_code{};
                std::filesystem::remove_all(m_path, error);
            }

            [[nodiscard]] auto source() const -> std::filesystem::path
            {
                return m_path / "source";
            }

            [[nodiscard]] auto build() const -> std::filesystem::path
            {
                return m_path / "build";
            }
        };

        [[nodiscard]]
        auto messageOf(Status const& status) -> std::string
        {
            if (status.has_value())
            {
                return std::string{};
            }
            return std::string{status.error().message()};
        }

        auto writeFile(
            std::filesystem::path const& path,
            std::string_view text
        ) -> void
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(
                path.parent_path(),
                error
            );
            REQUIRE_FALSE(error);

            auto stream = std::ofstream{
                path,
                std::ios::binary | std::ios::trunc
            };
            REQUIRE(stream.is_open());
            stream << text;
            REQUIRE(stream.good());
        }

        [[nodiscard]]
        auto snapshotTree(
            std::filesystem::path const& root
        ) -> std::map<std::string, std::string>
        {
            auto snapshot = std::map<std::string, std::string>{};
            for (auto const& entry : std::filesystem::recursive_directory_iterator{root})
            {
                if (!entry.is_regular_file())
                {
                    continue;
                }

                auto stream = std::ifstream{entry.path(), std::ios::binary};
                REQUIRE(stream.is_open());
                auto const bytes = std::string{
                    std::istreambuf_iterator<char>{stream},
                    std::istreambuf_iterator<char>{}
                };
                snapshot.emplace(
                    entry.path().lexically_relative(root).generic_string(),
                    bytes
                );
            }
            return snapshot;
        }

        [[nodiscard]]
        auto initializedWorkspace(
            TemporaryWorkspace const& workspace
        ) -> Status
        {
            writeFile(workspace.source() / "content" / "facts.txt", "facts\n");
            writeFile(workspace.source() / "decisions.txt", "decisions\n");
            return initProject(
                ProjectInitSpec{
                    .sourceDirectory = workspace.source(),
                    .buildDirectory  = workspace.build(),
                    .inputs          = {
                        "decisions.txt",
                        "content/facts.txt",
                    },
                }
            );
        }
    }

    TEST_CASE("project init records canonical declared inputs outside the source tree")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-init"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const sourceSnapshot = snapshotTree(workspace.source());
        REQUIRE_FALSE_MESSAGE(
            sourceSnapshot.contains(std::string{k_inputManifestName}),
            "project init must not write its input manifest into the source tree"
        );

        auto const snapshot = snapshotTree(workspace.build());
        REQUIRE_MESSAGE(
            snapshot.contains(std::string{k_inputManifestName}),
            "project init must write its input manifest into the build directory"
        );
        CHECK_MESSAGE(
            snapshot.at(std::string{k_inputManifestName})
            == "umbraflow-project-kit-inputs-v1\n"
               "content/facts.txt\n"
               "decisions.txt\n",
            "project init must record declared inputs in canonical sorted order"
        );
    }

    TEST_CASE("project build changes no source bytes and writes its receipt under build")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-build-boundary"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const sourceBefore = snapshotTree(workspace.source());

        auto const built = buildProject(
            ProjectDirectories{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = workspace.build(),
            }
        );
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        REQUIRE_MESSAGE(
            snapshotTree(workspace.source()) == sourceBefore,
            "project build must not change any source file"
        );
        CHECK_MESSAGE(
            std::filesystem::is_regular_file(
                workspace.build() / k_buildReceiptName
            ),
            "project build must write its receipt under the build directory"
        );
    }

    TEST_CASE("project check names a declared input removed after build")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-missing-input"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const directories = ProjectDirectories{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        };
        auto const built = buildProject(directories);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        REQUIRE(std::filesystem::remove(
            workspace.source() / "content" / "facts.txt"
        ));
        auto const checked = checkProject(directories);

        REQUIRE_FALSE_MESSAGE(
            checked.has_value(),
            "project check must reject a removed declared input"
        );
        CHECK_MESSAGE(
            messageOf(checked).find("content/facts.txt") != std::string::npos,
            "missing-input diagnostic must name content/facts.txt"
        );
        CHECK_MESSAGE(
            messageOf(checked).find("is missing") != std::string::npos,
            "missing-input diagnostic must state that the input is missing"
        );
    }

    TEST_CASE("project check rejects a build receipt for different declared inputs")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-stale-receipt"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));
        auto const directories = ProjectDirectories{
            .sourceDirectory = workspace.source(),
            .buildDirectory  = workspace.build(),
        };
        auto const built = buildProject(directories);
        REQUIRE_MESSAGE(built.has_value(), messageOf(built));

        writeFile(
            workspace.build() / k_buildReceiptName,
            "umbraflow-project-kit-build-v1\ndecisions.txt\n"
        );
        auto const checked = checkProject(directories);

        REQUIRE_FALSE_MESSAGE(
            checked.has_value(),
            "project check must reject a receipt for different declared inputs"
        );
        CHECK_MESSAGE(
            messageOf(checked).find("does not match declared inputs")
                != std::string::npos,
            "receipt mismatch diagnostic must name the declared-input mismatch"
        );
    }

    TEST_CASE("project build refuses a build directory inside the source tree")
    {
        auto const workspace   = TemporaryWorkspace{"uf-project-overlap"};
        auto const initialized = initializedWorkspace(workspace);
        REQUIRE_MESSAGE(initialized.has_value(), messageOf(initialized));

        auto const nestedBuild = workspace.source() / "generated";
        auto error             = std::error_code{};
        REQUIRE(std::filesystem::create_directories(nestedBuild, error));
        REQUIRE_FALSE(error);
        REQUIRE(std::filesystem::copy_file(
            workspace.build() / k_inputManifestName,
            nestedBuild / k_inputManifestName,
            error
        ));
        REQUIRE_FALSE(error);

        auto const built = buildProject(
            ProjectDirectories{
                .sourceDirectory = workspace.source(),
                .buildDirectory  = nestedBuild,
            }
        );

        REQUIRE_FALSE_MESSAGE(
            built.has_value(),
            "project build must reject a build directory inside the source tree"
        );
        CHECK_MESSAGE(
            messageOf(built).find("must be separate") != std::string::npos,
            "overlap diagnostic must name the source/build separation rule"
        );
        CHECK_FALSE_MESSAGE(
            std::filesystem::exists(nestedBuild / k_buildReceiptName),
            "overlap refusal must happen before a source-side receipt is written"
        );
    }
}
