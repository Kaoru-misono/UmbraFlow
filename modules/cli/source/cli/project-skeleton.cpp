#include "project-skeleton.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <array>
#include <filesystem>
#include <format>
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
        // here: page-model.toml is content and not layout.
        constexpr auto k_skeletonDirectories = std::array<std::string_view, 3>{
            "assets/templates",
            "assets/screens",
            "frames",
        };
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
        return ok();
    }
}
