#include "project-kit.hpp"

#include "declarative-single-step-tool.hpp"

#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <set>
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
        constexpr auto k_declarativeToolDirectory = std::string_view{
            "declarative-tools"
        };
        constexpr auto k_generatedDirectory = std::string_view{"generated"};
        constexpr auto k_generatedAdapterDirectory = std::string_view{
            "adapters"
        };

        struct GeneratedAdapter final
        {
            std::filesystem::path relativePath{};
            std::string           bytes{};
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
        auto generatedAdapters(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs
        ) -> Result<std::vector<GeneratedAdapter>>
        {
            auto adapters = std::vector<GeneratedAdapter>{};
            for (auto const& input : inputs)
            {
                auto const inputPath = std::filesystem::path{input};
                auto components      = std::vector<std::filesystem::path>{};
                for (auto const& component : inputPath)
                {
                    components.emplace_back(component);
                }
                if (
                    components.empty()
                    || components.front() != k_declarativeToolDirectory
                )
                {
                    continue;
                }
                if (
                    components.size() != 3U
                    || components.back().extension() != ".json"
                    || components.back().stem().empty()
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "declared single-step tool input must be "
                            "declarative-tools/<plugin-id>/<name>.json: \"{}\"",
                            input
                        )
                    );
                }

                auto const pluginId = components[1].generic_string();
                UF_TRY_VALUE(
                    declaration,
                    readText(
                        sourceDirectory / inputPath,
                        "single-step declaration"
                    )
                );
                UF_TRY_VALUE_CONTEXT(
                    adapter,
                    generateDeclarativeSingleStepAdapter(pluginId, declaration),
                    std::format(
                        "generating adapter from declared input \"{}\"",
                        input
                    )
                );
                auto adapterName = components.back();
                adapterName.replace_extension(".luau");
                adapters.emplace_back(
                    GeneratedAdapter{
                        .relativePath = std::filesystem::path{pluginId}
                            / adapterName,
                        .bytes        = std::move(adapter),
                    }
                );
            }
            return adapters;
        }

        [[nodiscard]]
        auto generatedAdapterRoot(
            std::filesystem::path const& buildDirectory
        ) -> std::filesystem::path
        {
            return buildDirectory
                / k_generatedDirectory
                / k_generatedAdapterDirectory;
        }

        [[nodiscard]]
        auto generatedAdapterName(
            std::filesystem::path const& relativePath
        ) -> std::string
        {
            return (
                std::filesystem::path{k_generatedDirectory}
                / k_generatedAdapterDirectory
                / relativePath
            ).generic_string();
        }

        [[nodiscard]]
        auto replaceGeneratedAdapterDirectory(
            std::filesystem::path const& buildDirectory
        ) -> Status
        {
            UF_TRY_VALUE(build, resolvedPath(buildDirectory, "build"));
            auto const adapterRoot = generatedAdapterRoot(build);
            if (!isWithinOrEqual(adapterRoot, build))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated adapter directory leaves the project build tree"
                );
            }

            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(
                adapterRoot,
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
                        "cannot inspect generated adapter directory \"{}\": {}",
                        adapterRoot.string(),
                        error.message()
                    )
                );
            }
            if (!error && std::filesystem::is_symlink(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated adapter directory must not be a link: \"{}\"",
                        adapterRoot.string()
                    )
                );
            }

            error = std::error_code{};
            std::filesystem::remove_all(adapterRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot replace generated adapter directory \"{}\": {}",
                        adapterRoot.string(),
                        error.message()
                    )
                );
            }
            error = std::error_code{};
            std::filesystem::create_directories(adapterRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create generated adapter directory \"{}\": {}",
                        adapterRoot.string(),
                        error.message()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto writeGeneratedAdapters(
            std::filesystem::path const& buildDirectory,
            std::vector<GeneratedAdapter> const& adapters
        ) -> Status
        {
            auto const adapterRoot = generatedAdapterRoot(buildDirectory);
            for (auto const& adapter : adapters)
            {
                auto const path = adapterRoot / adapter.relativePath;
                auto error      = std::error_code{};
                std::filesystem::create_directories(
                    path.parent_path(),
                    error
                );
                if (error)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        std::format(
                            "cannot create generated adapter parent \"{}\": {}",
                            path.parent_path().string(),
                            error.message()
                        )
                    );
                }
                UF_TRY(writeText(path, adapter.bytes, "generated adapter"));
            }
            return ok();
        }

        [[nodiscard]]
        auto expectedAdapterDirectories(
            std::set<std::string> const& files
        ) -> std::set<std::string>
        {
            auto directories = std::set<std::string>{};
            for (auto const& file : files)
            {
                auto current = std::filesystem::path{file}.parent_path();
                while (!current.empty())
                {
                    directories.emplace(current.generic_string());
                    current = current.parent_path();
                }
            }
            return directories;
        }

        [[nodiscard]]
        auto validateGeneratedAdapters(
            std::filesystem::path const& buildDirectory,
            std::vector<GeneratedAdapter> const& adapters
        ) -> Status
        {
            auto const adapterRoot = generatedAdapterRoot(buildDirectory);
            auto expectedFiles     = std::set<std::string>{};
            for (auto const& adapter : adapters)
            {
                expectedFiles.emplace(adapter.relativePath.generic_string());
            }
            auto const expectedDirectories = expectedAdapterDirectories(
                expectedFiles
            );
            auto actualFiles = std::set<std::string>{};

            auto error    = std::error_code{};
            auto iterator = std::filesystem::recursive_directory_iterator{
                adapterRoot,
                std::filesystem::directory_options::none,
                error,
            };
            if (error)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated project artifact directory \"{}\" is missing",
                        generatedAdapterName({})
                    )
                );
            }
            auto const end = std::filesystem::recursive_directory_iterator{};
            for (; !error && iterator != end; iterator.increment(error))
            {
                auto const status = iterator->symlink_status(error);
                if (error)
                {
                    break;
                }
                auto const relative = iterator->path().lexically_relative(
                    adapterRoot
                );
                auto const spelling     = relative.generic_string();
                auto const artifactName = generatedAdapterName(relative);
                if (std::filesystem::is_symlink(status))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated project artifact \"{}\" must not be a link",
                            artifactName
                        )
                    );
                }
                if (std::filesystem::is_directory(status))
                {
                    if (!expectedDirectories.contains(spelling))
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "generated project artifact \"{}\" has no "
                                "declared source",
                                artifactName
                            )
                        );
                    }
                    continue;
                }
                if (
                    status.type() != std::filesystem::file_type::regular
                    || !expectedFiles.contains(spelling)
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated project artifact \"{}\" has no "
                            "declared source",
                            artifactName
                        )
                    );
                }
                actualFiles.emplace(spelling);
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot enumerate generated adapter directory \"{}\": {}",
                        adapterRoot.string(),
                        error.message()
                    )
                );
            }

            for (auto const& adapter : adapters)
            {
                auto const spelling = adapter.relativePath.generic_string();
                if (!actualFiles.contains(spelling))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated project artifact \"{}\" is missing",
                            generatedAdapterName(adapter.relativePath)
                        )
                    );
                }
                auto const path = adapterRoot / adapter.relativePath;
                UF_TRY_VALUE(bytes, readText(path, "generated adapter"));
                if (bytes != adapter.bytes)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated project artifact \"{}\" does not match "
                            "its declared source",
                            generatedAdapterName(adapter.relativePath)
                        )
                    );
                }
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
        UF_TRY_VALUE(
            adapters,
            generatedAdapters(directories.sourceDirectory, inputs)
        );
        UF_TRY(ensureBuildDirectory(directories.buildDirectory));
        UF_TRY(replaceGeneratedAdapterDirectory(directories.buildDirectory));
        UF_TRY(writeGeneratedAdapters(directories.buildDirectory, adapters));

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
        UF_TRY_VALUE(
            adapters,
            generatedAdapters(directories.sourceDirectory, inputs)
        );
        UF_TRY(validateGeneratedAdapters(directories.buildDirectory, adapters));

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
