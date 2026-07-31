#include "path-validation.hpp"

#include "platform/windows-path.hpp"

#include <domain/error.hpp>

#include <filesystem>
#include <format>
#include <string>
#include <string_view>
#include <system_error>

namespace uf::input_agent
{
    namespace
    {
        [[nodiscard]]
        auto pathFailure(
            std::string_view operation,
            std::filesystem::path const& path,
            std::error_code error
        ) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "cannot {} path {}: {}",
                    operation,
                    path.string(),
                    error.message()
                )
            );
        }
    }

    auto canonicalizePathForComparison(
        std::filesystem::path const& path,
        std::string_view role
    ) -> Result<std::filesystem::path>
    {
        if (path.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("{} path must not be empty", role)
            );
        }

        auto error = std::error_code{};
        auto const absolute = std::filesystem::absolute(path, error);
        if (error)
        {
            return pathFailure("resolve", path, error);
        }

        auto canonical = std::filesystem::weakly_canonical(absolute, error);
        if (error)
        {
            return pathFailure("canonicalize", path, error);
        }
        return canonical;
    }

    auto canonicalizeOutputDirectory(
        std::filesystem::path const& path
    ) -> Result<std::filesystem::path>
    {
        UF_TRY_VALUE(
            canonical,
            canonicalizePathForComparison(
                path,
                "input-agent output directory"
            )
        );

        auto error = std::error_code{};
        auto const isDirectory = std::filesystem::is_directory(
            canonical,
            error
        );
        if (error)
        {
            return pathFailure("inspect output directory", canonical, error);
        }
        if (!isDirectory)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "input-agent output directory {} must be an existing "
                    "directory",
                    path.string()
                )
            );
        }
        return canonical;
    }

    auto isPathWithinDirectory(
        std::filesystem::path const& canonicalPath,
        std::filesystem::path const& canonicalDirectory
    ) -> bool
    {
        auto pathComponent = canonicalPath.begin();
        for (auto const& directoryComponent : canonicalDirectory)
        {
            if (
                pathComponent == canonicalPath.end()
                || !platform::pathsEqualOrdinal(
                    *pathComponent,
                    directoryComponent
                )
            )
            {
                return false;
            }
            ++pathComponent;
        }
        return true;
    }

    auto resolveConfinedOutputPath(
        std::filesystem::path const& canonicalOutputDirectory,
        std::filesystem::path const& output,
        std::string_view role
    ) -> Result<std::filesystem::path>
    {
        if (output.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format("{} path must not be empty", role)
            );
        }
        for (auto const& component : output)
        {
            if (component == std::filesystem::path{".."})
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} path {} contains forbidden parent traversal",
                        role,
                        output.string()
                    )
                );
            }
        }
        if (!output.is_absolute() && output.has_root_path())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} path {} has an unsupported drive-relative root",
                    role,
                    output.string()
                )
            );
        }

        auto candidate = canonicalOutputDirectory / output;
        if (output.is_absolute())
        {
            candidate = output;
        }
        auto const filename = candidate.filename();
        if (
            filename.empty()
            || filename == std::filesystem::path{"."}
            || filename == std::filesystem::path{".."}
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} path {} must name a file strictly inside the output "
                    "directory",
                    role,
                    output.string()
                )
            );
        }
        if (filename.native().contains(L':'))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} path {} must not name an alternate data stream",
                    role,
                    output.string()
                )
            );
        }

        auto error = std::error_code{};
        auto const canonicalParent = std::filesystem::canonical(
            candidate.parent_path(),
            error
        );
        if (error)
        {
            return pathFailure("canonicalize parent of", output, error);
        }
        auto const parentIsDirectory = std::filesystem::is_directory(
            canonicalParent,
            error
        );
        if (error)
        {
            return pathFailure("inspect parent of", output, error);
        }
        if (!parentIsDirectory)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} path {} must have an existing directory parent",
                    role,
                    output.string()
                )
            );
        }
        if (!isPathWithinDirectory(canonicalParent, canonicalOutputDirectory))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} path {} escapes input-agent output directory {}",
                    role,
                    output.string(),
                    canonicalOutputDirectory.string()
                )
            );
        }

        auto resolved = canonicalParent / filename;
        auto const status = std::filesystem::symlink_status(resolved, error);
        if (error == std::errc::no_such_file_or_directory)
        {
            return resolved;
        }
        if (error)
        {
            return pathFailure("inspect", resolved, error);
        }
        if (status.type() != std::filesystem::file_type::not_found)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "{} path {} already exists; input-agent outputs must be "
                    "fresh files",
                    role,
                    output.string()
                )
            );
        }
        return resolved;
    }

    auto canonicalPathsAlias(
        std::filesystem::path const& left,
        std::filesystem::path const& right
    ) -> Result<bool>
    {
        if (platform::pathsEqualOrdinal(left, right))
        {
            return true;
        }

        auto error = std::error_code{};
        auto const leftExists = std::filesystem::exists(left, error);
        if (error)
        {
            return pathFailure("inspect", left, error);
        }
        auto const rightExists = std::filesystem::exists(right, error);
        if (error)
        {
            return pathFailure("inspect", right, error);
        }
        if (!leftExists || !rightExists)
        {
            return false;
        }

        auto const equivalent = std::filesystem::equivalent(left, right, error);
        if (error)
        {
            return pathFailure("compare", left, error);
        }
        return equivalent;
    }
}
