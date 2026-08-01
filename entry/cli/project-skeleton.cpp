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
        // The three directories a project is authored INTO, project-relative.
        //
        // Each one is named by something else already, and this list is a fourth
        // spelling of none of them -- it is the one place that says a directory
        // has to EXIST, which is a different fact from where a file goes:
        //
        //   * assets/templates is where a measured crop is stored, under the hash
        //     of its own bytes (`scribe.template_path`).
        //   * assets/screens is where the pixels a screen is measured from live
        //     (`oracle.screen_path`), and it is the directory `check` replays.
        //   * frames is where a session keeps the captures it worked from. It is
        //     the one nothing in this binary ever reads -- an agent writes into it
        //     through project_write and a developer reads it afterwards -- and it
        //     is laid out for exactly that reason: an agent that has to create it
        //     cannot, because creating a directory is not a verb the script layer
        //     has.
        //
        // Nothing else belongs here. page-model.toml is content and not layout:
        // it states the geometry the project was authored at, which is a fact
        // about the target rather than about the directory, and a file this
        // function invented would be a model nobody wrote.
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
            // false return is not an error on its own -- it means nothing was
            // created -- so the error code is what is asked.
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
