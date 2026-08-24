#include "project-skeleton.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <task/runtime-model-file.hpp>

#include <array>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::cli
{
    namespace
    {
        // The three directories a project is authored INTO, project-relative. Each is
        // named elsewhere by the code that writes into it; this list is the one place
        // saying a directory has to EXIST, which is a different fact from where a
        // file goes: privileged annotation writes crops into assets/templates,
        // assets/screens holds its offline corpus, and frames holds the captures a
        // session worked from. Nothing else belongs
        // here: runtime-model.toml is content and not layout.
        constexpr auto k_skeletonDirectories = std::array<std::string_view, 3>{
            "assets/templates",
            "assets/screens",
            "frames",
        };
    }

    namespace
    {
        // Writes one file if, and only if, nothing is there. An existing file
        // is left exactly as it is: the bytes a project has are the project's,
        // and this function's whole job is to make an empty directory workable
        // rather than to decide what an authored one should say.
        [[nodiscard]]
        auto writeIfMissing(
            std::filesystem::path const& path,
            std::string_view bytes
        ) -> Status
        {
            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(path, error);
            if (error && error != std::errc::no_such_file_or_directory)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect \"{}\": {}",
                        path.string(),
                        error.message()
                    )
                );
            }
            if (status.type() != std::filesystem::file_type::not_found)
            {
                return ok();
            }

            error = std::error_code{};
            static_cast<void>(
                std::filesystem::create_directories(path.parent_path(), error)
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create \"{}\": {}",
                        path.parent_path().string(),
                        error.message()
                    )
                );
            }

            auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
            stream.write(
                bytes.data(),
                static_cast<std::streamsize>(bytes.size())
            );
            stream.flush();
            if (!stream)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format("cannot write \"{}\"", path.string())
                );
            }
            return ok();
        }

        // The genesis RuntimeArtifact, for a project that has none. Its bytes
        // are the framework's constant rather than this file's, so a project
        // initialised here and a project scaffolded by `project init` start
        // from the same H_genesis.
        [[nodiscard]]
        auto ensureGenesisArtifact(
            std::filesystem::path const& projectRoot
        ) -> Status
        {
            auto const artifactRoot = projectRoot
                / std::filesystem::path{k_projectRuntimeArtifactPath};
            UF_TRY_VALUE(manifest, task::genesisRuntimeArtifactManifestJcs());
            UF_TRY(writeIfMissing(
                artifactRoot / std::string{task::k_runtimeModelFileName},
                task::k_genesisRuntimeModelToml
            ));
            return writeIfMissing(
                artifactRoot
                    / std::string{task::k_runtimeArtifactManifestFileName},
                manifest
            );
        }
    }

    auto ensureProjectSkeleton(std::filesystem::path const& projectRoot) -> Status
    {
        auto error = std::error_code{};
        if (!std::filesystem::is_directory(projectRoot, error))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "there is no project directory at \"{}\"; this lays out the "
                    "directories a project is authored into and does not create "
                    "the project itself",
                    projectRoot.string()
                )
            );
        }

        for (auto const relative : k_skeletonDirectories)
        {
            auto const directory = projectRoot / std::filesystem::path{relative};

            error             = std::error_code{};
            auto const status = std::filesystem::status(directory, error);
            if (error && error != std::errc::no_such_file_or_directory)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect \"{}\": {}",
                        directory.string(),
                        error.message()
                    )
                );
            }
            if (std::filesystem::is_directory(status))
            {
                continue;
            }
            if (status.type() != std::filesystem::file_type::not_found)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "\"{}\" already exists and is not a directory, so this "
                        "project cannot hold the {} an authoring session writes "
                        "into",
                        directory.string(),
                        relative
                    )
                );
            }

            error = std::error_code{};
            // create_directories rather than create_directory, because two of the
            // three are nested and their shared parent may be missing too. Its
            // false return means nothing was created, not an error, so the error
            // code is what is asked.
            static_cast<void>(std::filesystem::create_directories(directory, error));
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create \"{}\": {}",
                        directory.string(),
                        error.message()
                    )
                );
            }
        }

        return ensureGenesisArtifact(projectRoot);
    }
}
