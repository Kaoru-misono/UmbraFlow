#include "project-kit.hpp"

#include "declarative-workflow-tool.hpp"
#include "tool-catalog.hpp"

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <image/png.hpp>
#include <image/template-cut.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <algorithm>
#include <bit>
#include <cstddef>
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <ranges>
#include <set>
#include <span>
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
        constexpr auto k_artifactManifestSchema = std::string_view{
            "umbraflow-project-kit-artifact-manifest/v1"
        };
        constexpr auto k_artifactRegistrationSchema = std::string_view{
            "umbraflow-project-kit-artifact-registration/v1"
        };
        constexpr auto k_declarativeToolDirectory = std::string_view{
            "declarative-tools"
        };
        // The one document a project directory holds at its root, and the one
        // statement of its shape. The kit cannot link the runtime loader --
        // uf::deployment reaches uf::task, and the `project` executable links
        // uf::project and uf::core alone -- so the shape lives in neither of
        // them: it is published under schema/ and reaches both through the
        // framework schema catalog this module already embeds. There is no
        // second, narrower reading of this document here.
        constexpr auto k_projectManifestName = std::string_view{
            "umbraflow-project.json"
        };
        constexpr auto k_projectSchemaPath = std::string_view{
            "schema/umbraflow-project-v1.schema.json"
        };
        constexpr auto k_generatedDirectory = std::string_view{"generated"};
        constexpr auto k_generatedAdapterDirectory = std::string_view{
            "adapters"
        };
        constexpr auto k_generatedToolCatalogDirectory = std::string_view{
            "tool-catalogs"
        };
        constexpr auto k_generatedTemplateDirectory = std::string_view{
            "templates"
        };
        constexpr auto k_generatedFrameworkSchemaDirectory = std::string_view{
            "framework-schemas"
        };
        constexpr auto k_generatedArtifactBlobDirectory = std::string_view{
            "artifact-blobs"
        };
        constexpr auto k_generatedRegistrationDirectory = std::string_view{
            "registrations"
        };
        constexpr auto k_toolCatalogName = std::string_view{
            "tool-catalog-v1.json"
        };
        constexpr auto k_frameworkSchemaCatalogName = std::string_view{
            "framework-schema-catalog-v1.json"
        };
        constexpr auto k_artifactRegistrationName = std::string_view{
            "artifact-roots-v1.json"
        };

        struct GeneratedArtifact final
        {
            std::filesystem::path relativePath{};
            std::string           bytes{};
        };

        struct GeneratedArtifactFamily final
        {
            std::string                    directory{};
            std::vector<GeneratedArtifact> artifacts{};
        };

        struct GeneratedProjectBuild final
        {
            std::vector<GeneratedArtifactFamily> families{};
        };

        struct ManifestRow final
        {
            std::string path{};
            std::string digest{};
            std::size_t size{};
        };

        struct ArtifactManifest final
        {
            std::vector<ManifestRow> inputs{};
            std::vector<ManifestRow> artifacts{};
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

            error          = std::error_code{};
            auto canonical = std::filesystem::weakly_canonical(
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
            ProjectBuildSpec const& spec
        ) -> Status
        {
            UF_TRY(requireDirectory(spec.sourceDirectory, "source"));
            UF_TRY_VALUE(
                source,
                resolvedPath(spec.sourceDirectory, "source")
            );
            UF_TRY_VALUE(
                build,
                resolvedPath(spec.buildDirectory, "build")
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
                spec.buildDirectory,
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
                        spec.buildDirectory.string(),
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
                        spec.buildDirectory.string()
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

            auto text = std::string{
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

        // The project's root document, judged by the one published statement of
        // its shape.
        //
        // It is read from the source tree unconditionally. The declared-input
        // list is the author's, so a gate that ran only when the author had
        // listed umbraflow-project.json was one the author could switch off by
        // not listing it, while the tree still held the document the runtime
        // loader would later read.
        //
        // Nothing here reads a member. schema/umbraflow-project-v1.schema.json
        // states every rule, including the direct-plugin tier's admission gate:
        // a deployment whose plugin_authoring is "hand-written" must carry a
        // plugin_justification naming the member or semantic of
        // umbraflow-declarative-workflow-tool/v1 that cannot express it, and
        // one whose plugin_authoring is "generated" must carry none, because a
        // generated adapter IS that tier and has nothing to justify.
        // uf::deployment's loader compiles the same published bytes, so the two
        // readers cannot reach two verdicts.
        //
        // THAT GATE CHECKS PRESENCE, NOT TRUTH. It cannot tell whether a stated
        // reason is correct, and does not try: judging that would require
        // deciding whether a Luau module is equivalent to some declaration,
        // which is program equivalence. A justification that names the wrong
        // member is a review finding at plugin acceptance -- see
        // docs/pitfalls/checks-that-cannot-fail.md.
        [[nodiscard]]
        auto validateProjectManifest(
            std::filesystem::path const& sourceDirectory
        ) -> Status
        {
            auto const manifestPath = (
                sourceDirectory / std::filesystem::path{k_projectManifestName}
            );
            auto error        = std::error_code{};
            auto const status = std::filesystem::status(manifestPath, error);
            if (
                error == std::errc::no_such_file_or_directory
                || status.type() == std::filesystem::file_type::not_found
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "a project needs {} at the root of its source "
                        "directory, and \"{}\" holds none",
                        k_projectManifestName,
                        sourceDirectory.string()
                    )
                );
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect project root document \"{}\": {}",
                        manifestPath.string(),
                        error.message()
                    )
                );
            }
            if (!std::filesystem::is_regular_file(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project root document is not a regular file: \"{}\"",
                        manifestPath.string()
                    )
                );
            }
            UF_TRY_VALUE(bytes, readText(manifestPath, "root document"));

            auto const published = framework_schema::findFrameworkSchema(
                k_projectSchemaPath
            );
            if (!published.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated framework schema catalog is missing "
                        + std::string{k_projectSchemaPath}
                );
            }
            auto const compiled = json::Schema::compile(json::Schema::Document{
                .label      = published->relativePath,
                .exactBytes = published->exactBytes,
            });
            if (!compiled.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} is not a schema this kit can apply: {}",
                        published->relativePath,
                        compiled.error().message()
                    )
                );
            }
            UF_TRY_VALUE_CONTEXT(
                document,
                json::parse(bytes),
                std::format("reading {}", k_projectManifestName)
            );
            auto const judged = compiled->validate(document);
            if (!judged.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{}: {}",
                        k_projectManifestName,
                        judged.error().message()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto generatedAdapters(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs
        ) -> Result<std::vector<GeneratedArtifact>>
        {
            auto adapters = std::vector<GeneratedArtifact>{};
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
                            "declared workflow tool input must be "
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
                        "workflow declaration"
                    )
                );
                UF_TRY_VALUE_CONTEXT(
                    adapter,
                    generateDeclarativeWorkflowAdapter(pluginId, declaration),
                    std::format(
                        "generating adapter from declared input \"{}\"",
                        input
                    )
                );
                auto adapterName = components.back();
                adapterName.replace_extension(".luau");
                adapters.emplace_back(
                    GeneratedArtifact{
                        .relativePath = std::filesystem::path{pluginId}
                            / adapterName,
                        .bytes        = std::move(adapter),
                    }
                );
            }
            return adapters;
        }

        [[nodiscard]]
        auto generatedToolCatalogs(
            std::vector<ToolCatalogDeclaration> const& declarations
        ) -> Result<std::vector<GeneratedArtifact>>
        {
            auto catalogs  = std::vector<GeneratedArtifact>{};
            auto pluginIds = std::set<std::string>{};
            catalogs.reserve(declarations.size());
            for (auto const& declaration : declarations)
            {
                if (!pluginIds.emplace(declaration.pluginId).second)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated Tool Catalog declared plugin {} more than once",
                            declaration.pluginId
                        )
                    );
                }
                UF_TRY_VALUE_CONTEXT(
                    catalog,
                    generateToolCatalog(declaration),
                    std::format(
                        "generating Tool Catalog for plugin {}",
                        declaration.pluginId
                    )
                );
                catalogs.emplace_back(GeneratedArtifact{
                    .relativePath = std::filesystem::path{declaration.pluginId}
                        / k_toolCatalogName,
                    .bytes = std::move(catalog),
                });
            }
            return catalogs;
        }

        [[nodiscard]]
        auto byteString(std::span<std::byte const> bytes) -> std::string
        {
            auto output = std::string{};
            output.reserve(bytes.size());
            for (auto const value : bytes)
            {
                output.push_back(std::bit_cast<char>(value));
            }
            return output;
        }

        [[nodiscard]]
        auto normalizeTemplatePath(
            std::filesystem::path const& path
        ) -> Result<std::filesystem::path>
        {
            auto normalized = path.lexically_normal();
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
                        "generated template path must be relative: \"{}\"",
                        path.string()
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
                            "generated template path leaves its artifact "
                            "family: \"{}\"",
                            path.string()
                        )
                    );
                }
            }
            return normalized;
        }

        [[nodiscard]]
        auto generatedTemplates(
            std::vector<ProjectTemplateCutSpec> const& declarations,
            TemplateSourceResolver const& resolveTemplateSource
        ) -> Result<std::vector<GeneratedArtifact>>
        {
            if (!declarations.empty() && !resolveTemplateSource)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project template cuts require a content-hash resolver"
                );
            }

            auto templates = std::vector<GeneratedArtifact>{};
            templates.reserve(declarations.size());
            for (auto const& declaration : declarations)
            {
                if (declaration.sourceHashes.empty())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated template \"{}\" requires at least "
                            "one source hash",
                            declaration.templatePath.string()
                        )
                    );
                }
                UF_TRY_VALUE(
                    templatePath,
                    normalizeTemplatePath(declaration.templatePath)
                );

                auto sources = std::vector<image::RgbaImage>{};
                sources.reserve(declaration.sourceHashes.size());
                for (auto const& sourceHash : declaration.sourceHashes)
                {
                    UF_TRY_VALUE_CONTEXT(
                        encoded,
                        resolveTemplateSource(sourceHash),
                        std::format(
                            "resolving template source {}",
                            sourceHash.hex()
                        )
                    );
                    UF_TRY_VALUE(actualHash, sha256(encoded));
                    if (actualHash != sourceHash)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "resolved template source {} has content hash {}",
                                sourceHash.hex(),
                                actualHash.hex()
                            )
                        );
                    }
                    UF_TRY_VALUE(
                        source,
                        image::decodePng(encoded, sourceHash.hex())
                    );
                    sources.emplace_back(std::move(source));
                }

                UF_TRY_VALUE(
                    cut,
                    image::cutRgba8Template(sources, declaration.rect)
                );
                UF_TRY_VALUE(
                    encodedTemplate,
                    image::encodeRgbaPng(
                        templatePath.generic_string(),
                        cut.image.width,
                        cut.image.height,
                        cut.image.pixels
                    )
                );
                templates.emplace_back(GeneratedArtifact{
                    .relativePath = std::move(templatePath),
                    .bytes        = byteString(encodedTemplate),
                });
            }

            std::ranges::sort(templates, {}, &GeneratedArtifact::relativePath);
            auto const duplicate = std::ranges::adjacent_find(
                templates,
                {},
                &GeneratedArtifact::relativePath
            );
            if (duplicate != templates.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated template path appears more than once: \"{}\"",
                        duplicate->relativePath.string()
                    )
                );
            }
            return templates;
        }

        [[nodiscard]]
        auto generatedFrameworkSchemaCatalog()
            -> std::vector<GeneratedArtifact>
        {
            auto documents    = std::vector<json::Value>{};
            auto const catalog = framework_schema::frameworkSchemaCatalog();
            documents.reserve(catalog.size());
            for (auto const& document : catalog)
            {
                documents.emplace_back(json::Value::ofObject({
                    {
                        "identity",
                        json::Value::ofString(std::string{document.identity}),
                    },
                    {
                        "path",
                        json::Value::ofString(std::string{document.relativePath}),
                    },
                    {
                        "sha256",
                        json::Value::ofString(std::string{document.sha256}),
                    },
                }));
            }

            auto artifacts = std::vector<GeneratedArtifact>{};
            artifacts.emplace_back(GeneratedArtifact{
                .relativePath = k_frameworkSchemaCatalogName,
                .bytes = json::canonicalBytes(json::Value::ofObject({
                    {"documents", json::Value::ofArray(std::move(documents))},
                    {
                        "schema",
                        json::Value::ofString(
                            "umbraflow-framework-schema-catalog/v1"
                        ),
                    },
                })),
            });
            return artifacts;
        }

        [[nodiscard]]
        auto generatedArtifactClosure(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs,
            std::vector<ProjectArtifactBlobSpec> const& declarations,
            ProjectRegistrationBuildSpec const& registration
        ) -> Result<std::vector<GeneratedArtifactFamily>>
        {
            auto normalizedDeclarations = declarations;
            for (auto& declaration : normalizedDeclarations)
            {
                UF_TRY_VALUE(
                    normalized,
                    normalizeInputPath(declaration.sourceInput)
                );
                if (!std::ranges::binary_search(inputs, normalized))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project artifact blob \"{}\" names undeclared "
                            "source input \"{}\"",
                            declaration.name,
                            normalized
                        )
                    );
                }
                declaration.sourceInput = std::filesystem::path{normalized};
            }
            std::ranges::sort(
                normalizedDeclarations,
                {},
                &ProjectArtifactBlobSpec::name
            );

            auto registeredNames = registration.artifactBlobNames;
            std::ranges::sort(registeredNames);
            for (auto const& registeredName : registeredNames)
            {
                auto const declared = std::ranges::lower_bound(
                    normalizedDeclarations,
                    registeredName,
                    {},
                    &ProjectArtifactBlobSpec::name
                );
                if (
                    declared == normalizedDeclarations.end()
                    || declared->name != registeredName
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project registration artifact blob \"{}\" is "
                            "outside the RuntimeArtifact closure",
                            registeredName
                        )
                    );
                }
            }

            auto blobs = std::vector<GeneratedArtifact>{};
            auto roots = std::vector<json::Value>{};
            blobs.reserve(normalizedDeclarations.size());
            roots.reserve(normalizedDeclarations.size());
            for (auto const& declaration : normalizedDeclarations)
            {
                if (!std::ranges::binary_search(registeredNames, declaration.name))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "RuntimeArtifact closure blob \"{}\" is absent "
                            "from the project registration",
                            declaration.name
                        )
                    );
                }
                UF_TRY_VALUE(
                    bytes,
                    readText(
                        sourceDirectory / declaration.sourceInput,
                        "artifact blob source"
                    )
                );
                UF_TRY_VALUE(
                    digest,
                    sha256(std::as_bytes(std::span{bytes}))
                );
                roots.emplace_back(json::Value::ofObject({
                    {"name", json::Value::ofString(declaration.name)},
                    {"sha256", json::Value::ofString(digest.hex())},
                }));
                blobs.emplace_back(GeneratedArtifact{
                    .relativePath = declaration.name + ".blob",
                    .bytes        = std::move(bytes),
                });
            }

            auto registrations = std::vector<GeneratedArtifact>{};
            registrations.emplace_back(GeneratedArtifact{
                .relativePath = k_artifactRegistrationName,
                .bytes = json::canonicalBytes(json::Value::ofObject({
                    {"artifact_roots", json::Value::ofArray(std::move(roots))},
                    {
                        "schema",
                        json::Value::ofString(
                            std::string{k_artifactRegistrationSchema}
                        ),
                    },
                })),
            });
            auto families = std::vector<GeneratedArtifactFamily>{};
            families.emplace_back(GeneratedArtifactFamily{
                .directory = std::string{k_generatedArtifactBlobDirectory},
                .artifacts = std::move(blobs),
            });
            families.emplace_back(GeneratedArtifactFamily{
                .directory = std::string{k_generatedRegistrationDirectory},
                .artifacts = std::move(registrations),
            });
            return families;
        }

        [[nodiscard]]
        auto generatedProjectBuild(
            ProjectBuildSpec const& spec,
            std::vector<std::string> const& inputs,
            TemplateSourceResolver const& resolveTemplateSource
        ) -> Result<GeneratedProjectBuild>
        {
            UF_TRY_VALUE(
                adapters,
                generatedAdapters(spec.sourceDirectory, inputs)
            );
            UF_TRY_VALUE(catalogs, generatedToolCatalogs(spec.toolCatalogs));
            UF_TRY_VALUE(
                templates,
                generatedTemplates(spec.templateCuts, resolveTemplateSource)
            );
            UF_TRY_VALUE(
                closureFamilies,
                generatedArtifactClosure(
                    spec.sourceDirectory,
                    inputs,
                    spec.artifactBlobs,
                    spec.registration
                )
            );

            // RuntimeArtifact membership begins at generator output. Source
            // inputs, including a hand-written plugin, are digest-pinned only.
            auto families = std::vector<GeneratedArtifactFamily>{};
            families.emplace_back(GeneratedArtifactFamily{
                .directory = std::string{k_generatedAdapterDirectory},
                .artifacts = std::move(adapters),
            });
            families.emplace_back(GeneratedArtifactFamily{
                .directory = std::string{k_generatedToolCatalogDirectory},
                .artifacts = std::move(catalogs),
            });
            families.emplace_back(GeneratedArtifactFamily{
                .directory = std::string{k_generatedTemplateDirectory},
                .artifacts = std::move(templates),
            });
            families.emplace_back(GeneratedArtifactFamily{
                .directory = std::string{k_generatedFrameworkSchemaDirectory},
                .artifacts = generatedFrameworkSchemaCatalog(),
            });
            for (auto& family : closureFamilies)
            {
                families.emplace_back(std::move(family));
            }
            return GeneratedProjectBuild{.families = std::move(families)};
        }

        [[nodiscard]]
        auto generatedArtifactRoot(
            std::filesystem::path const& buildDirectory,
            std::string_view familyDirectory
        ) -> std::filesystem::path
        {
            return buildDirectory
                / k_generatedDirectory
                / familyDirectory;
        }

        [[nodiscard]]
        auto generatedArtifactName(
            std::string_view familyDirectory,
            std::filesystem::path const& relativePath
        ) -> std::string
        {
            return (
                std::filesystem::path{k_generatedDirectory}
                / familyDirectory
                / relativePath
            ).generic_string();
        }

        [[nodiscard]]
        auto replaceGeneratedArtifactDirectory(
            std::filesystem::path const& buildDirectory,
            std::string_view familyDirectory
        ) -> Status
        {
            UF_TRY_VALUE(build, resolvedPath(buildDirectory, "build"));
            auto const artifactRoot = generatedArtifactRoot(
                build,
                familyDirectory
            );
            if (!isWithinOrEqual(artifactRoot, build))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated artifact directory leaves the project build tree"
                );
            }

            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(
                artifactRoot,
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
                        "cannot inspect generated artifact directory \"{}\": {}",
                        artifactRoot.string(),
                        error.message()
                    )
                );
            }
            if (!error && std::filesystem::is_symlink(status))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated artifact directory must not be a link: \"{}\"",
                        artifactRoot.string()
                    )
                );
            }
            error = std::error_code{};
            std::filesystem::remove_all(artifactRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot replace generated artifact directory \"{}\": {}",
                        artifactRoot.string(),
                        error.message()
                    )
                );
            }
            error = std::error_code{};
            std::filesystem::create_directories(artifactRoot, error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create generated artifact directory \"{}\": {}",
                        artifactRoot.string(),
                        error.message()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto writeGeneratedArtifacts(
            std::filesystem::path const& buildDirectory,
            std::string_view familyDirectory,
            std::vector<GeneratedArtifact> const& artifacts
        ) -> Status
        {
            auto const artifactRoot = generatedArtifactRoot(
                buildDirectory,
                familyDirectory
            );
            for (auto const& artifact : artifacts)
            {
                auto const path = artifactRoot / artifact.relativePath;
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
                            "cannot create generated artifact parent \"{}\": {}",
                            path.parent_path().string(),
                            error.message()
                        )
                    );
                }
                UF_TRY(writeText(path, artifact.bytes, "generated artifact"));
            }
            return ok();
        }

        [[nodiscard]]
        auto writeGeneratedProjectBuild(
            std::filesystem::path const& buildDirectory,
            GeneratedProjectBuild const& generated
        ) -> Status
        {
            for (auto const& family : generated.families)
            {
                UF_TRY(replaceGeneratedArtifactDirectory(
                    buildDirectory,
                    family.directory
                ));
                UF_TRY(writeGeneratedArtifacts(
                    buildDirectory,
                    family.directory,
                    family.artifacts
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto expectedArtifactDirectories(
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
        auto validateGeneratedArtifacts(
            std::filesystem::path const& buildDirectory,
            std::string_view familyDirectory,
            std::vector<GeneratedArtifact> const& artifacts
        ) -> Status
        {
            auto const artifactRoot = generatedArtifactRoot(
                buildDirectory,
                familyDirectory
            );
            auto expectedFiles     = std::set<std::string>{};
            for (auto const& artifact : artifacts)
            {
                expectedFiles.emplace(artifact.relativePath.generic_string());
            }
            auto const expectedDirectories = expectedArtifactDirectories(
                expectedFiles
            );
            auto actualFiles = std::set<std::string>{};

            auto error    = std::error_code{};
            auto iterator = std::filesystem::recursive_directory_iterator{
                artifactRoot,
                std::filesystem::directory_options::none,
                error,
            };
            if (error)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated project artifact directory \"{}\" is missing",
                        generatedArtifactName(familyDirectory, {})
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
                    artifactRoot
                );
                auto const spelling     = relative.generic_string();
                auto const artifactName = generatedArtifactName(
                    familyDirectory,
                    relative
                );
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
                if (std::filesystem::is_regular_file(status))
                {
                    auto const links = std::filesystem::hard_link_count(
                        iterator->path(),
                        error
                    );
                    if (error)
                    {
                        break;
                    }
                    if (links != 1U)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "generated project artifact \"{}\" must not be a link",
                                artifactName
                            )
                        );
                    }
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
                        "cannot enumerate generated artifact directory \"{}\": {}",
                        artifactRoot.string(),
                        error.message()
                    )
                );
            }

            for (auto const& artifact : artifacts)
            {
                auto const spelling = artifact.relativePath.generic_string();
                if (!actualFiles.contains(spelling))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated project artifact \"{}\" is missing",
                            generatedArtifactName(
                                familyDirectory,
                                artifact.relativePath
                            )
                        )
                    );
                }
                auto const path = artifactRoot / artifact.relativePath;
                UF_TRY_VALUE(bytes, readText(path, "generated artifact"));
                if (bytes != artifact.bytes)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "generated project artifact \"{}\" does not match "
                            "its declared source",
                            generatedArtifactName(
                                familyDirectory,
                                artifact.relativePath
                            )
                        )
                    );
                }
            }
            return ok();
        }

        [[nodiscard]]
        auto validateGeneratedProjectBuild(
            std::filesystem::path const& buildDirectory,
            GeneratedProjectBuild const& generated
        ) -> Status
        {
            for (auto const& family : generated.families)
            {
                UF_TRY(validateGeneratedArtifacts(
                    buildDirectory,
                    family.directory,
                    family.artifacts
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto manifestRow(
            std::string path,
            std::string_view bytes
        ) -> Result<ManifestRow>
        {
            UF_TRY_VALUE(digest, sha256(std::as_bytes(std::span{bytes})));
            return ManifestRow{
                .path   = std::move(path),
                .digest = digest.hex(),
                .size   = bytes.size(),
            };
        }

        [[nodiscard]]
        auto manifestRowsValue(
            std::vector<ManifestRow> const& rows
        ) -> json::Value
        {
            auto values = std::vector<json::Value>{};
            values.reserve(rows.size());
            for (auto const& row : rows)
            {
                values.emplace_back(json::Value::ofObject({
                    {"path", json::Value::ofString(row.path)},
                    {"sha256", json::Value::ofString(row.digest)},
                    {
                        "size",
                        json::Value::ofString(std::to_string(row.size)),
                    },
                }));
            }
            return json::Value::ofArray(std::move(values));
        }

        [[nodiscard]]
        auto createArtifactManifest(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs,
            GeneratedProjectBuild const& generated
        ) -> Result<ArtifactManifest>
        {
            auto manifest = ArtifactManifest{};
            manifest.inputs.reserve(inputs.size());
            for (auto const& input : inputs)
            {
                UF_TRY_VALUE(
                    bytes,
                    readText(
                        sourceDirectory / std::filesystem::path{input},
                        "artifact manifest input"
                    )
                );
                UF_TRY_VALUE(row, manifestRow(input, bytes));
                manifest.inputs.emplace_back(std::move(row));
            }

            for (auto const& family : generated.families)
            {
                for (auto const& artifact : family.artifacts)
                {
                    UF_TRY_VALUE(
                        row,
                        manifestRow(
                            generatedArtifactName(
                                family.directory,
                                artifact.relativePath
                            ),
                            artifact.bytes
                        )
                    );
                    manifest.artifacts.emplace_back(std::move(row));
                }
            }
            std::ranges::sort(manifest.inputs, {}, &ManifestRow::path);
            std::ranges::sort(manifest.artifacts, {}, &ManifestRow::path);
            return manifest;
        }

        [[nodiscard]]
        auto renderArtifactManifest(ArtifactManifest const& manifest) -> std::string
        {
            return json::canonicalBytes(json::Value::ofObject({
                {"artifacts", manifestRowsValue(manifest.artifacts)},
                {"inputs", manifestRowsValue(manifest.inputs)},
                {
                    "schema",
                    json::Value::ofString(std::string{k_artifactManifestSchema}),
                },
            }));
        }

        [[nodiscard]]
        auto releaseArtifactRows(
            std::string_view manifestBytes
        ) -> Result<std::vector<ManifestRow>>
        {
            UF_TRY(json::requireExactCanonical(manifestBytes));
            UF_TRY_VALUE(document, json::parse(manifestBytes));
            auto const* schema    = document.find("schema");
            auto const* artifacts = document.find("artifacts");
            if (
                schema == nullptr
                || schema->string() != k_artifactManifestSchema
                || artifacts == nullptr
                || artifacts->kind() != json::ValueKind::Array
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project release artifact manifest has the wrong shape"
                );
            }

            auto rows = std::vector<ManifestRow>{};
            rows.reserve(artifacts->items().size());
            for (auto const& item : artifacts->items())
            {
                auto const* path   = item.find("path");
                auto const* digest = item.find("sha256");
                if (
                    path == nullptr
                    || path->kind() != json::ValueKind::String
                    || digest == nullptr
                    || digest->kind() != json::ValueKind::String
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "project release artifact manifest row has the wrong shape"
                    );
                }
                UF_TRY_VALUE(normalized, normalizeInputPath(path->string()));
                if (normalized != path->string())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project release artifact path is not canonical: \"{}\"",
                            path->string()
                        )
                    );
                }
                rows.emplace_back(ManifestRow{
                    .path   = std::move(normalized),
                    .digest = std::string{digest->string()},
                    .size   = 0U,
                });
            }
            std::ranges::sort(rows, {}, &ManifestRow::path);
            auto const duplicate = std::ranges::adjacent_find(
                rows,
                {},
                &ManifestRow::path
            );
            if (duplicate != rows.end())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project release artifact appears more than once: \"{}\"",
                        duplicate->path
                    )
                );
            }
            return rows;
        }

        [[nodiscard]]
        auto makeReadOnly(
            std::filesystem::path const& releaseDirectory
        ) -> Status
        {
            auto entries = std::vector<std::filesystem::path>{};
            auto error   = std::error_code{};
            auto iterator = std::filesystem::recursive_directory_iterator{
                releaseDirectory,
                std::filesystem::directory_options::none,
                error,
            };
            auto const end = std::filesystem::recursive_directory_iterator{};
            for (; !error && iterator != end; iterator.increment(error))
            {
                entries.emplace_back(iterator->path());
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot enumerate project release \"{}\": {}",
                        releaseDirectory.string(),
                        error.message()
                    )
                );
            }

            std::ranges::sort(
                entries,
                {},
                [](std::filesystem::path const& path)
                {
                    return path.native().size();
                }
            );
            for (auto const& entry : entries | std::views::reverse)
            {
                error = std::error_code{};
                auto const status = std::filesystem::status(entry, error);
                if (error)
                {
                    break;
                }
                auto const permissions = (
                    std::filesystem::is_directory(status)
                        ? std::filesystem::perms::owner_read
                            | std::filesystem::perms::owner_exec
                            | std::filesystem::perms::group_read
                            | std::filesystem::perms::group_exec
                            | std::filesystem::perms::others_read
                            | std::filesystem::perms::others_exec
                        : std::filesystem::perms::owner_read
                            | std::filesystem::perms::group_read
                            | std::filesystem::perms::others_read
                );
                std::filesystem::permissions(
                    entry,
                    permissions,
                    std::filesystem::perm_options::replace,
                    error
                );
                if (error)
                {
                    break;
                }
            }
            if (!error)
            {
                std::filesystem::permissions(
                    releaseDirectory,
                    std::filesystem::perms::owner_read
                        | std::filesystem::perms::owner_exec
                        | std::filesystem::perms::group_read
                        | std::filesystem::perms::group_exec
                        | std::filesystem::perms::others_read
                        | std::filesystem::perms::others_exec,
                    std::filesystem::perm_options::replace,
                    error
                );
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot make project release read-only \"{}\": {}",
                        releaseDirectory.string(),
                        error.message()
                    )
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto validateReleaseTree(
            std::filesystem::path const& releaseDirectory,
            std::vector<ManifestRow> const& rows
        ) -> Status
        {
            auto error = std::error_code{};
            auto const releaseStatus = std::filesystem::status(
                releaseDirectory,
                error
            );
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect project release \"{}\": {}",
                        releaseDirectory.string(),
                        error.message()
                    )
                );
            }
            if (
                (
                    releaseStatus.permissions()
                    & std::filesystem::perms::owner_write
                ) != std::filesystem::perms::none
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project release directory is not read-only"
                );
            }
            auto expectedFiles = std::set<std::string>{
                std::string{k_artifactManifestName},
            };
            for (auto const& row : rows)
            {
                expectedFiles.emplace(row.path);
            }
            auto const expectedDirectories = expectedArtifactDirectories(
                expectedFiles
            );
            auto actualFiles = std::set<std::string>{};

            auto iterator = std::filesystem::recursive_directory_iterator{
                releaseDirectory,
                std::filesystem::directory_options::none,
                error,
            };
            auto const end = std::filesystem::recursive_directory_iterator{};
            for (; !error && iterator != end; iterator.increment(error))
            {
                auto const status = iterator->symlink_status(error);
                if (error)
                {
                    break;
                }
                auto const name = iterator->path()
                    .lexically_relative(releaseDirectory)
                    .generic_string();
                if (std::filesystem::is_symlink(status))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project release artifact \"{}\" must not be a link",
                            name
                        )
                    );
                }
                if (std::filesystem::is_directory(status))
                {
                    if (!expectedDirectories.contains(name))
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "project release contains artifact outside its "
                                "manifest: \"{}\"",
                                name
                            )
                        );
                    }
                }
                else if (
                    !std::filesystem::is_regular_file(status)
                    || !expectedFiles.contains(name)
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project release contains artifact outside its "
                            "manifest: \"{}\"",
                            name
                        )
                    );
                }
                else
                {
                    actualFiles.emplace(name);
                }

                if (
                    (
                        status.permissions()
                        & std::filesystem::perms::owner_write
                    ) != std::filesystem::perms::none
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project release artifact is not read-only: \"{}\"",
                            name
                        )
                    );
                }
            }
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot enumerate project release \"{}\": {}",
                        releaseDirectory.string(),
                        error.message()
                    )
                );
            }
            if (!actualFiles.contains(std::string{k_artifactManifestName}))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project release artifact manifest is missing"
                );
            }
            for (auto const& row : rows)
            {
                if (!actualFiles.contains(row.path))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project release artifact is missing: \"{}\"",
                            row.path
                        )
                    );
                }
                UF_TRY_VALUE(
                    bytes,
                    readText(releaseDirectory / row.path, "release artifact")
                );
                UF_TRY_VALUE(
                    digest,
                    sha256(std::as_bytes(std::span{bytes}))
                );
                if (digest.hex() != row.digest)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project release artifact digest does not match: \"{}\"",
                            row.path
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
            ProjectBuildSpec{
                .sourceDirectory = spec.sourceDirectory,
                .buildDirectory  = spec.buildDirectory,
                .toolCatalogs    = {},
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

    auto buildProject(
        ProjectBuildSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Status
    {
        UF_TRY(validateDirectories(spec));
        UF_TRY_VALUE(inputs, declaredInputs(spec.buildDirectory));
        UF_TRY(validateDeclaredInputs(spec.sourceDirectory, inputs));
        UF_TRY(validateProjectManifest(spec.sourceDirectory));
        UF_TRY_VALUE(
            generated,
            generatedProjectBuild(spec, inputs, resolveTemplateSource)
        );
        UF_TRY(ensureBuildDirectory(spec.buildDirectory));
        UF_TRY(writeGeneratedProjectBuild(spec.buildDirectory, generated));

        auto const receiptPath = spec.buildDirectory / k_buildReceiptName;
        UF_TRY(writeText(
            receiptPath,
            renderList(k_buildReceiptHeader, inputs),
            "build receipt"
        ));
        UF_TRY_VALUE(
            manifest,
            createArtifactManifest(spec.sourceDirectory, inputs, generated)
        );
        return writeText(
            spec.buildDirectory / k_artifactManifestName,
            renderArtifactManifest(manifest),
            "artifact manifest"
        );
    }

    auto checkProject(
        ProjectBuildSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Status
    {
        UF_TRY(validateDirectories(spec));
        UF_TRY_VALUE(inputs, declaredInputs(spec.buildDirectory));
        UF_TRY(validateDeclaredInputs(spec.sourceDirectory, inputs));
        UF_TRY(validateProjectManifest(spec.sourceDirectory));
        UF_TRY_VALUE(
            generated,
            generatedProjectBuild(spec, inputs, resolveTemplateSource)
        );
        UF_TRY(validateGeneratedProjectBuild(spec.buildDirectory, generated));

        auto const receiptPath = spec.buildDirectory / k_buildReceiptName;
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
        UF_TRY_VALUE(
            manifest,
            createArtifactManifest(spec.sourceDirectory, inputs, generated)
        );
        auto const manifestPath = spec.buildDirectory / k_artifactManifestName;
        UF_TRY_VALUE(actual, readText(manifestPath, "artifact manifest"));
        if (actual != renderArtifactManifest(manifest))
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "project artifact manifest does not match the complete "
                    "input pins and RuntimeArtifact closure: \"{}\"",
                    manifestPath.string()
                )
            );
        }
        return ok();
    }

    auto freezeProject(
        ProjectFreezeSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Result<std::filesystem::path>
    {
        UF_TRY(checkProject(spec.candidate, resolveTemplateSource));
        auto const manifestPath = (
            spec.candidate.buildDirectory / k_artifactManifestName
        );
        UF_TRY_VALUE(
            manifestBytes,
            readText(manifestPath, "artifact manifest")
        );
        UF_TRY_VALUE(rows, releaseArtifactRows(manifestBytes));
        UF_TRY_VALUE(
            releaseDigest,
            sha256(std::as_bytes(std::span{manifestBytes}))
        );
        auto releaseDirectory = spec.releaseRoot / releaseDigest.hex();

        auto error = std::error_code{};
        if (std::filesystem::is_directory(releaseDirectory, error))
        {
            UF_TRY(loadProjectRelease(releaseDirectory));
            return releaseDirectory;
        }
        if (error && error != std::errc::no_such_file_or_directory)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot inspect project release \"{}\": {}",
                    releaseDirectory.string(),
                    error.message()
                )
            );
        }

        error = std::error_code{};
        std::filesystem::create_directories(releaseDirectory, error);
        if (error)
        {
            return fail(
                AutomationErrorKind::IoFailure,
                std::format(
                    "cannot create project release \"{}\": {}",
                    releaseDirectory.string(),
                    error.message()
                )
            );
        }
        for (auto const& row : rows)
        {
            auto const source = spec.candidate.buildDirectory / row.path;
            auto const target = releaseDirectory / row.path;
            error             = std::error_code{};
            std::filesystem::create_directories(target.parent_path(), error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create project release artifact parent \"{}\": {}",
                        target.parent_path().string(),
                        error.message()
                    )
                );
            }
            UF_TRY_VALUE(bytes, readText(source, "candidate artifact"));
            UF_TRY(writeText(target, bytes, "release artifact"));
        }
        UF_TRY(writeText(
            releaseDirectory / k_artifactManifestName,
            manifestBytes,
            "release artifact manifest"
        ));
        UF_TRY(makeReadOnly(releaseDirectory));
        UF_TRY(loadProjectRelease(releaseDirectory));
        return releaseDirectory;
    }

    auto loadProjectRelease(
        std::filesystem::path const& releaseDirectory
    ) -> Status
    {
        UF_TRY(requireDirectory(releaseDirectory, "release"));
        UF_TRY_VALUE(
            manifestBytes,
            readText(
                releaseDirectory / k_artifactManifestName,
                "release artifact manifest"
            )
        );
        UF_TRY_VALUE(
            releaseDigest,
            sha256(std::as_bytes(std::span{manifestBytes}))
        );
        if (releaseDirectory.filename().string() != releaseDigest.hex())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "project release id does not match its artifact manifest"
            );
        }
        UF_TRY_VALUE(rows, releaseArtifactRows(manifestBytes));
        return validateReleaseTree(releaseDirectory, rows);
    }
}
