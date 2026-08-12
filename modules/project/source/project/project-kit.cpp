#include "project-kit.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        constexpr auto k_inputManifestHeader = std::string_view{
            "umbraflow-project-kit-inputs-v1"
        };
        constexpr auto k_buildReceiptHeader = std::string_view{
            "umbraflow-project-kit-build-v1"
        };

        [[nodiscard]]
        auto requireDirectory(
            std::filesystem::path const& directory,
            std::string_view role
        ) -> Status
        {
            auto error             = std::error_code{};
            auto const isDirectory = std::filesystem::is_directory(
                directory,
                error
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect project {} directory \"{}\": {}",
                        role,
                        directory.string(),
                        error.message()
                    )
                );
            }
            if (!isDirectory)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project {} directory does not exist: \"{}\"",
                        role,
                        directory.string()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto resolvedPath(
            std::filesystem::path const& path,
            std::string_view role
        ) -> Result<std::filesystem::path>
        {
            auto error          = std::error_code{};
            auto const absolute = std::filesystem::absolute(path, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot resolve project {} path \"{}\": {}",
                        role,
                        path.string(),
                        error.message()
                    )
                );
            }

            error                = std::error_code{};
            auto const canonical = std::filesystem::weakly_canonical(
                absolute,
                error
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot canonicalize project {} path \"{}\": {}",
                        role,
                        path.string(),
                        error.message()
                    )
                );
            }
            return canonical;
        }

        [[nodiscard]]
        auto isWithinOrEqual(
            std::filesystem::path const& candidate,
            std::filesystem::path const& root
        ) -> bool
        {
            auto candidatePart = candidate.begin();
            for (auto const& rootPart : root)
            {
                if (
                    candidatePart == candidate.end()
                    || *candidatePart != rootPart
                )
                {
                    return false;
                }
                ++candidatePart;
            }
            return true;
        }

        [[nodiscard]]
        auto validateDirectories(
            ProjectDirectories const& directories
        ) -> Status
        {
            UF_TRY(requireDirectory(directories.sourceDirectory, "source"));
            UF_TRY_VALUE(
                source,
                resolvedPath(directories.sourceDirectory, "source")
            );
            UF_TRY_VALUE(
                build,
                resolvedPath(directories.buildDirectory, "build")
            );

            if (isWithinOrEqual(build, source) || isWithinOrEqual(source, build))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project source and build directories must be separate: "
                        "source=\"{}\", build=\"{}\"",
                        source.string(),
                        build.string()
                    )
                );
            }

            auto error = std::error_code{};
            auto const status = std::filesystem::status(
                directories.buildDirectory,
                error
            );
            if (
                error
                && error != std::errc::no_such_file_or_directory
            )
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect project build directory \"{}\": {}",
                        directories.buildDirectory.string(),
                        error.message()
                    )
                );
            }
            if (
                !error
                && status.type() != std::filesystem::file_type::not_found
                && !std::filesystem::is_directory(status)
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project build path is not a directory: \"{}\"",
                        directories.buildDirectory.string()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto ensureBuildDirectory(
            std::filesystem::path const& buildDirectory
        ) -> Status
        {
            auto error = std::error_code{};
            std::filesystem::create_directories(
                buildDirectory,
                error
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create project build directory \"{}\": {}",
                        buildDirectory.string(),
                        error.message()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto normalizeInputPath(
            std::filesystem::path const& input
        ) -> Result<std::string>
        {
            auto const normalized = input.lexically_normal();
            if (
                normalized.empty()
                || normalized == std::filesystem::path{"."}
                || normalized.is_absolute()
                || normalized.has_root_name()
                || normalized.has_root_directory()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "declared project input must be a relative file path: "
                        "\"{}\"",
                        input.string()
                    )
                );
            }

            for (auto const& component : normalized)
            {
                if (component == std::filesystem::path{".."})
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "declared project input leaves the source tree: "
                            "\"{}\"",
                            input.string()
                        )
                    );
                }
            }
            return normalized.generic_string();
        }

        [[nodiscard]]
        auto validateDeclaredInput(
            std::filesystem::path const& sourceDirectory,
            std::string_view input
        ) -> Status
        {
            auto const inputPath = sourceDirectory / std::filesystem::path{input};
            auto error           = std::error_code{};
            auto const status    = std::filesystem::status(inputPath, error);
            if (
                error == std::errc::no_such_file_or_directory
                || status.type() == std::filesystem::file_type::not_found
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "declared project input \"{}\" is missing at \"{}\"",
                        input,
                        inputPath.string()
                    )
                );
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect declared project input \"{}\" at \"{}\": {}",
                        input,
                        inputPath.string(),
                        error.message()
                    )
                );
            }
            if (!std::filesystem::is_regular_file(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "declared project input \"{}\" is not a regular file at \"{}\"",
                        input,
                        inputPath.string()
                    )
                );
            }

            UF_TRY_VALUE(source, resolvedPath(sourceDirectory, "source"));
            UF_TRY_VALUE(resolvedInput, resolvedPath(inputPath, "input"));
            if (!isWithinOrEqual(resolvedInput, source))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "declared project input \"{}\" resolves outside the "
                        "source tree at \"{}\"",
                        input,
                        resolvedInput.string()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto canonicalInputs(
            ProjectInitSpec const& spec
        ) -> Result<std::vector<std::string>>
        {
            if (spec.inputs.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project init requires at least one declared input"
                );
            }

            auto inputs = std::vector<std::string>{};
            inputs.reserve(spec.inputs.size());
            for (auto const& input : spec.inputs)
            {
                UF_TRY_VALUE(normalized, normalizeInputPath(input));
                UF_TRY(validateDeclaredInput(spec.sourceDirectory, normalized));
                inputs.emplace_back(std::move(normalized));
            }

            std::ranges::sort(inputs);
            auto const duplicate = std::ranges::adjacent_find(inputs);
            if (duplicate != inputs.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "declared project input appears more than once: \"{}\"",
                        *duplicate
                    )
                );
            }
            return inputs;
        }

        [[nodiscard]]
        auto readText(
            std::filesystem::path const& path,
            std::string_view role
        ) -> Result<std::string>
        {
            auto stream = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot open project {} \"{}\"",
                        role,
                        path.string()
                    )
                );
            }

            auto const text = std::string{
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{}
            };
            if (stream.bad())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot read project {} \"{}\"",
                        role,
                        path.string()
                    )
                );
            }
            return text;
        }

        [[nodiscard]]
        auto writeText(
            std::filesystem::path const& path,
            std::string_view text,
            std::string_view role
        ) -> Status
        {
            auto stream = std::ofstream{
                path,
                std::ios::binary | std::ios::trunc
            };
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot open project {} \"{}\" for writing",
                        role,
                        path.string()
                    )
                );
            }

            stream << text;
            if (!stream)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot write project {} \"{}\"",
                        role,
                        path.string()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto renderList(
            std::string_view header,
            std::vector<std::string> const& inputs
        ) -> std::string
        {
            auto text = std::string{header};
            text += '\n';
            for (auto const& input : inputs)
            {
                text += input;
                text += '\n';
            }
            return text;
        }

        [[nodiscard]]
        auto declaredInputs(
            std::filesystem::path const& buildDirectory
        ) -> Result<std::vector<std::string>>
        {
            auto const manifestPath = buildDirectory / k_inputManifestName;
            UF_TRY_VALUE(text, readText(manifestPath, "input manifest"));

            auto lines  = std::istringstream{text};
            auto header = std::string{};
            std::getline(lines, header);
            if (!header.empty() && header.back() == '\r')
            {
                header.pop_back();
            }
            if (header != k_inputManifestHeader)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project input manifest \"{}\" has an unsupported header",
                        manifestPath.string()
                    )
                );
            }

            auto inputs = std::vector<std::string>{};
            auto line   = std::string{};
            while (std::getline(lines, line))
            {
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                UF_TRY_VALUE(normalized, normalizeInputPath(line));
                if (normalized != line)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project input manifest path is not canonical: \"{}\"",
                            line
                        )
                    );
                }
                inputs.emplace_back(std::move(normalized));
            }
            if (inputs.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project input manifest \"{}\" declares no inputs",
                        manifestPath.string()
                    )
                );
            }

            if (!std::ranges::is_sorted(inputs))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project input manifest \"{}\" is not sorted",
                        manifestPath.string()
                    )
                );
            }
            auto const duplicate = std::ranges::adjacent_find(inputs);
            if (duplicate != inputs.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project input manifest repeats \"{}\"",
                        *duplicate
                    )
                );
            }
            return inputs;
        }

        [[nodiscard]]
        auto validateDeclaredInputs(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs
        ) -> Status
        {
            for (auto const& input : inputs)
            {
                UF_TRY(validateDeclaredInput(sourceDirectory, input));
            }
            return ok();
        }
    }

    auto initProject(ProjectInitSpec const& spec) -> Status
    {
        UF_TRY(validateDirectories(
            ProjectDirectories{
                .sourceDirectory = spec.sourceDirectory,
                .buildDirectory  = spec.buildDirectory,
            }
        ));
        UF_TRY_VALUE(inputs, canonicalInputs(spec));
        UF_TRY(ensureBuildDirectory(spec.buildDirectory));

        auto const manifestPath = spec.buildDirectory / k_inputManifestName;
        return writeText(
            manifestPath,
            renderList(k_inputManifestHeader, inputs),
            "input manifest"
        );
    }

    auto buildProject(ProjectDirectories const& directories) -> Status
    {
        UF_TRY(validateDirectories(directories));
        UF_TRY_VALUE(inputs, declaredInputs(directories.buildDirectory));
        UF_TRY(validateDeclaredInputs(directories.sourceDirectory, inputs));
        UF_TRY(ensureBuildDirectory(directories.buildDirectory));

        auto const receiptPath = directories.buildDirectory / k_buildReceiptName;
        return writeText(
            receiptPath,
            renderList(k_buildReceiptHeader, inputs),
            "build receipt"
        );
    }

    auto checkProject(ProjectDirectories const& directories) -> Status
    {
        UF_TRY(validateDirectories(directories));
        UF_TRY_VALUE(inputs, declaredInputs(directories.buildDirectory));
        UF_TRY(validateDeclaredInputs(directories.sourceDirectory, inputs));

        auto const receiptPath = directories.buildDirectory / k_buildReceiptName;
        UF_TRY_VALUE(receipt, readText(receiptPath, "build receipt"));
        auto const expected = renderList(k_buildReceiptHeader, inputs);
        if (receipt != expected)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "project build receipt does not match declared inputs: \"{}\"",
                    receiptPath.string()
                )
            );
        }
        return ok();
    }
}
