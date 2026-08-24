#include "project-kit.hpp"

#include "declarative-workflow-tool.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/utf8.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/png.hpp>
#include <image/template-cut.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <operator/manifest.hpp>
#include <operator/project-plugin.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <task/runtime-model-file.hpp>

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
        constexpr auto k_buildReceiptHeader = std::string_view{
            "umbraflow-project-kit-build-v1"
        };
        constexpr auto k_artifactManifestSchema = std::string_view{
            "umbraflow-project-kit-artifact-manifest/v1"
        };
        constexpr auto k_executionClosureSchema = std::string_view{
            "umbraflow-project-kit-execution-closure/v3"
        };

        // The one closure slot of a deployment, spelled once. It names the
        // staging subdirectory its modules are written under.
        constexpr auto k_toolClosureName = std::string_view{"tool"};
        constexpr auto k_declarativeToolDirectory = std::string_view{
            "declarative-tools"
        };
        constexpr auto k_generatedDirectory = std::string_view{"generated"};
        constexpr auto k_generatedAdapterDirectory = std::string_view{
            "adapters"
        };
        constexpr auto k_generatedTemplateDirectory = std::string_view{
            "templates"
        };
        constexpr auto k_generatedFrameworkSchemaDirectory = std::string_view{
            "framework-schemas"
        };
        constexpr auto k_generatedResourceDirectory = std::string_view{
            "resources"
        };
        constexpr auto k_generatedModuleDirectory = std::string_view{
            "modules"
        };
        constexpr auto k_generatedRegistrationDirectory = std::string_view{
            "registrations"
        };
        constexpr auto k_frameworkSchemaCatalogName = std::string_view{
            "framework-schema-catalog-v1.json"
        };

        // One `template_cuts` entry of umbraflow-project.json, after the
        // published schema has judged it and this file has turned its three
        // members into the values the rest of the kit works in. It is not in
        // the module's header because no caller assembles one: the document is
        // the only place a cut is declared, and a second way to supply one
        // would be a second spelling of the project's template cuts.
        struct ProjectTemplateCutSpec final
        {
            std::filesystem::path    templatePath{};
            std::vector<ContentHash> sourceHashes{};
            PixelRect                rect;
        };

        struct ProjectResourceSpec final
        {
            operator_runtime::ProjectResourceKind kind{
                operator_runtime::ProjectResourceKind::Json
            };
            std::string           name{};
            std::filesystem::path sourceInput{};
        };

        struct ProjectModuleSpec final
        {
            std::string           name{};
            std::filesystem::path sourceInput{};
        };

        // One closure of a deployment's registration generation: the entry
        // module its manifest digest is taken over, and the closed module
        // graph beneath it. Two of these make a deployment, and neither is
        // optional -- a project that binds no Tool declares a tool closure
        // whose exported entry set is empty rather than leaving the slot out.
        struct ProjectClosureBuildSpec final
        {
            std::string                    entryModule{};
            std::vector<std::string>       exportedEntryPoints{};
            std::vector<ProjectModuleSpec> modules{};
        };

        struct ProjectRegistrationBuildSpec final
        {
            std::string                      deploymentName{};
            std::string                      pluginId{};
            ProjectClosureBuildSpec          toolClosure{};
            std::vector<ProjectResourceSpec> resources{};
        };

        // Everything one schema-validated parse of umbraflow-project.json says
        // about what to build.
        //
        // `inputs` is derived here rather than read from anything on disk. It
        // used to be a ledger only `project init` wrote, which went stale the
        // moment a declaration named a module the ledger did not, and left
        // `project build` unable to run at all until an init that reached the
        // network had refreshed it. The declaration is the statement; the list
        // is arithmetic over it, and arithmetic is redone rather than cached.
        struct ProjectManifest final
        {
            std::vector<std::string>                  inputs{};
            std::vector<ProjectTemplateCutSpec>       templateCuts{};
            std::vector<ProjectRegistrationBuildSpec> registrations{};
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

        struct ScaffoldFile final
        {
            std::filesystem::path relativePath{};
            std::string           bytes{};
        };

        [[nodiscard]]
        auto jsonDocument(json::Value const& value) -> std::string
        {
            auto bytes = json::canonicalBytes(value);
            bytes.push_back('\n');
            return bytes;
        }

        // One scaffolded closure. The reducer carries the scaffold's support
        // module beside its entry so a starter project shows a closed graph
        // rather than a lone file; the tool closure carries only its entry,
        // because a scaffold binds no Tool and its declared entry set is
        // therefore explicitly empty.
        [[nodiscard]]
        auto scaffoldClosure(
            std::string const& pluginId,
            ProjectPluginForm form,
            std::string_view closureName,
            std::vector<std::string> const& exportedEntryPoints
        ) -> json::Value
        {
            auto modules = std::vector<json::Value>{};
            switch (form)
            {
            case ProjectPluginForm::Generated:
                modules.emplace_back(json::Value::ofObject({
                    {"name", json::Value::ofString("main")},
                    {
                        "path",
                        json::Value::ofString(
                            "generated/adapters/" + pluginId + "/scaffold/"
                            + std::string{closureName} + ".luau"
                        ),
                    },
                }));
                break;
            case ProjectPluginForm::HandWritten:
                modules.emplace_back(json::Value::ofObject({
                    {"name", json::Value::ofString("main")},
                    {"path", json::Value::ofString(
                        "plugin/" + std::string{closureName} + ".luau"
                    )},
                }));
                modules.emplace_back(json::Value::ofObject({
                    {"name", json::Value::ofString("support")},
                    {"path", json::Value::ofString("plugin/support.luau")},
                }));
                break;
            }
            auto entryPoints = std::vector<json::Value>{};
            entryPoints.reserve(exportedEntryPoints.size());
            for (auto const& entryPoint : exportedEntryPoints)
            {
                entryPoints.emplace_back(json::Value::ofString(entryPoint));
            }
            return json::Value::ofObject({
                {"entry", json::Value::ofString("main")},
                {"exported_entry_points", json::Value::ofArray(std::move(entryPoints))},
                {"modules", json::Value::ofArray(std::move(modules))},
            });
        }

        // The one Tool a scaffold declares, inline. Its argument_schema is a
        // real inline schema rather than `unchecked`, because a starter that
        // declined the guard would teach the shape by omission; a project that
        // wants no argument enforcement replaces the object with the string.
        [[nodiscard]]
        auto scaffoldTools(std::string const& pluginId) -> json::Value
        {
            return json::Value::ofArray({json::Value::ofObject({
                        {
                            "argument_schema",
                            json::Value::ofObject({
                                {
                                    "$schema",
                                    json::Value::ofString(
                                        "https://json-schema.org/draft/2020-12/schema"
                                    ),
                                },
                                {
                                    "type",
                                    json::Value::ofString("object"),
                                },
                            }),
                        },
                        {"body", json::Value::ofBoolean(false)},
                        {
                            // The empty declaration, written out. A scaffold
                            // tool calls nothing, and this is how a tool says
                            // so: no names, no calls, and the most restricted
                            // ceiling of each kind.
                            "child_effects",
                            json::Value::ofObject({
                                {"child_tool_names", json::Value::ofArray({})},
                                {
                                    "maximum_child_calls",
                                    json::Value::ofNumber(0),
                                },
                                {
                                    "maximum_child_mutability",
                                    json::Value::ofString("read_only"),
                                },
                                {
                                    "maximum_child_risk",
                                    json::Value::ofString("read_only"),
                                },
                                {
                                    "maximum_child_surface",
                                    json::Value::ofString("semantic"),
                                },
                            }),
                        },
                        {
                            "effect_bounds",
                            json::Value::ofArray({}),
                        },
                        {
                            "idempotency",
                            json::Value::ofString("delivery_safe"),
                        },
                        {
                            "mutability",
                            json::Value::ofString("read_only"),
                        },
                        {
                            "name",
                            json::Value::ofString(pluginId + ".scaffold"),
                        },
                        {
                            "required_capabilities",
                            json::Value::ofArray({}),
                        },
                        {"surface", json::Value::ofString("semantic")},
                        {
                            "timeout_policy",
                            json::Value::ofObject({
                                {
                                    "maximum_elapsed_ms",
                                    json::Value::ofNumber(3000),
                                },
                                {
                                    "on_timeout",
                                    json::Value::ofString("stop"),
                                },
                            }),
                        },
                        {
                            "ui_action_bounds",
                            json::Value::ofArray({}),
                        },
                        {"version", json::Value::ofString("1.0.0")},
                        {
                            "workflow_limits",
                            json::Value::ofObject({
                                {
                                    "maximum_dispatches",
                                    json::Value::ofNumber(1),
                                },
                                {
                                    "maximum_elapsed_ms",
                                    json::Value::ofNumber(3000),
                                },
                                {
                                    "maximum_observations",
                                    json::Value::ofNumber(1),
                                },
                                {
                                    "maximum_steps",
                                    json::Value::ofNumber(1),
                                },
                                {
                                    "maximum_waits",
                                    json::Value::ofNumber(1),
                                },
                            }),
                        },
            })});
        }

        [[nodiscard]]
        auto scaffoldProjectDocument(
            ProjectScaffoldSpec const& spec
        ) -> json::Value
        {
            auto deployment = std::vector<json::Member>{
                {"name", json::Value::ofString("main")},
                {
                    "observed_instance_identity_schemas",
                    json::Value::ofArray({}),
                },
                {
                    // A scaffold binds no Tool, so its tool closure states an
                    // empty export set. The slot is written out rather than
                    // omitted on the same terms as tool_bindings below.
                    "tool_closure",
                    scaffoldClosure(
                        spec.pluginId,
                        spec.pluginForm,
                        k_toolClosureName,
                        {}
                    ),
                },
                {
                    "plugin_authoring",
                    json::Value::ofString(
                        spec.pluginForm == ProjectPluginForm::Generated
                            ? "generated"
                            : "hand-written"
                    ),
                },
                {"plugin_id", json::Value::ofString(spec.pluginId)},
                {
                    "resources",
                    json::Value::ofArray({json::Value::ofObject({
                        {"kind", json::Value::ofString("utf8")},
                        {"name", json::Value::ofString("facts")},
                        {
                            "path",
                            json::Value::ofString("content/placeholder.txt"),
                        },
                    })}),
                },
                {
                    // A scaffold binds no Tool to a closure entry, and the
                    // empty array is the whole statement of that. It is
                    // written out rather than left out on the same terms as
                    // observed_instance_identity_schemas.
                    "tool_bindings",
                    json::Value::ofArray({}),
                },
                {"tools", scaffoldTools(spec.pluginId)},
            };
            if (spec.pluginForm == ProjectPluginForm::HandWritten)
            {
                deployment.emplace_back(
                    "plugin_justification",
                    json::Value::ofString(
                        "Replace this scaffold explanation with the member or "
                        "semantic that umbraflow-declarative-workflow-tool/v1 "
                        "cannot express."
                    )
                );
            }
            return json::Value::ofObject({
                {
                    "deployments",
                    json::Value::ofArray({
                        json::Value::ofObject(std::move(deployment)),
                    }),
                },
                {"primary_deployment", json::Value::ofString("main")},
                {"runtime_artifact", json::Value::ofString("runtime/artifact")},
                {
                    "schema",
                    json::Value::ofString(std::string{k_projectContractVersion}),
                },
                {"template_cuts", json::Value::ofArray({})},
            });
        }

        [[nodiscard]]
        auto scaffoldDeclarativeTool(
            std::string const& pluginId
        ) -> json::Value
        {
            return json::Value::ofObject({
                {
                    "allowed_instance_kinds",
                    json::Value::ofArray({
                        json::Value::ofString(pluginId + ".target"),
                    }),
                },
                {
                    "bounds",
                    json::Value::ofObject({
                        {
                            "maximum_dispatches",
                            json::Value::ofNumber(1),
                        },
                        {
                            "maximum_elapsed_ms",
                            json::Value::ofNumber(3000),
                        },
                        {
                            "maximum_observations",
                            json::Value::ofNumber(1),
                        },
                        {
                            "maximum_states",
                            json::Value::ofNumber(1),
                        },
                        {"maximum_steps", json::Value::ofNumber(1)},
                        {"maximum_waits", json::Value::ofNumber(1)},
                    }),
                },
                {
                    "fresh_observation",
                    json::Value::ofObject({
                        {
                            "required_surface",
                            json::Value::ofString(pluginId + ".surface"),
                        },
                        {
                            "require_unambiguous",
                            json::Value::ofBoolean(true),
                        },
                    }),
                },
                {
                    "schema",
                    json::Value::ofString(
                        "umbraflow-declarative-workflow-tool/v1"
                    ),
                },
                {
                    "states",
                    json::Value::ofArray({json::Value::ofObject({
                        {"kind", json::Value::ofString("wait")},
                        {"observation_budget", json::Value::ofNumber(1)},
                        {"state_key", json::Value::ofString("observe")},
                        {"timeout_ms", json::Value::ofNumber(2000)},
                    })}),
                },
                {
                    "steps",
                    json::Value::ofArray({json::Value::ofString("observe")}),
                },
                {
                    "target_argument",
                    json::Value::ofString("observed_instance_id"),
                },
                {
                    "tool_name",
                    json::Value::ofString(pluginId + ".scaffold"),
                },
                {
                    "ui_finding",
                    json::Value::ofObject({
                        {
                            "kind",
                            json::Value::ofString("observed_instance_absent"),
                        },
                    }),
                },
            });
        }

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

            if (isWithinOrEqual(source, build))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project source directory must not be inside the build "
                        "directory: source=\"{}\", build=\"{}\"",
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
        auto declarativeInputForGeneratedModule(
            std::string_view modulePath
        ) -> Result<std::string>
        {
            // One declaration renders both closures of one deployment, so the
            // closure slot is the last path component and the declaration is
            // the one above it. Both slots therefore map back to the same
            // declared input, which is what makes the input set a project
            // states independent of how many closures a generator emits.
            constexpr auto prefix = std::string_view{"generated/adapters/"};
            constexpr auto suffix = std::string_view{".luau"};
            auto const refuseShape = [modulePath]
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated Project module \"{}\" must map to "
                        "generated/adapters/<plugin>/<tool>/<closure>.luau",
                        modulePath
                    )
                );
            };
            if (!modulePath.starts_with(prefix) || !modulePath.ends_with(suffix))
            {
                return refuseShape();
            }
            auto const body = modulePath.substr(
                prefix.size(),
                modulePath.size() - prefix.size() - suffix.size()
            );
            auto const slot = body.rfind('/');
            if (slot == std::string_view::npos || slot == 0U)
            {
                return refuseShape();
            }
            auto const closureName = body.substr(slot + 1U);
            if (closureName != k_toolClosureName)
            {
                return refuseShape();
            }
            return std::string{k_declarativeToolDirectory}
                + "/" + std::string{body.substr(0U, slot)} + ".json";
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
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& declared
        ) -> Result<std::vector<std::string>>
        {
            auto inputs = std::vector<std::string>{};
            inputs.reserve(declared.size());
            for (auto const& input : declared)
            {
                UF_TRY_VALUE(normalized, normalizeInputPath(input));
                UF_TRY(validateDeclaredInput(sourceDirectory, normalized));
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

        [[nodiscard]]
        auto member(json::Value const& object, std::string_view name)
            -> json::Value const&;

        [[nodiscard]]
        auto writeProjectFileIfMissing(
            std::filesystem::path const& path,
            std::string_view bytes
        ) -> Status
        {
            auto error = std::error_code{};
            if (std::filesystem::exists(path, error))
                return ok();
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect starter Project file \"{}\": {}",
                        path.string(),
                        error.message()
                    )
                );
            }
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot create starter Project file directory \"{}\": {}",
                        path.parent_path().string(),
                        error.message()
                    )
                );
            }
            return writeText(path, bytes, "starter Project file");
        }

        // The source files this declaration names, complete.
        //
        // Every entry is arithmetic over the document: the root document
        // itself, each deployment's resources, and each module of its closure
        // -- or, for a generated deployment, the declaration each generated
        // module is rendered from. There is no way for a project to add a file
        // to this set except by declaring it, which is what makes the set
        // recomputable after any edit with nothing to refresh.
        [[nodiscard]]
        auto derivedInputs(
            std::filesystem::path const& sourceDirectory,
            json::Value const& document
        ) -> Result<std::vector<std::string>>
        {
            auto inputs = std::set<std::string>{};
            inputs.emplace(std::string{k_projectManifestName});

            for (auto const& deployment : member(document, "deployments").items())
            {
                auto const isGenerated = (
                    member(deployment, "plugin_authoring").string()
                    == "generated"
                );
                auto const& closure = member(deployment, "tool_closure");
                for (auto const& module : member(closure, "modules").items())
                {
                    UF_TRY_VALUE(
                        normalized,
                        normalizeInputPath(
                            std::filesystem::path{member(module, "path").string()}
                        )
                    );
                    if (isGenerated)
                    {
                        UF_TRY_VALUE(
                            declaration,
                            declarativeInputForGeneratedModule(normalized)
                        );
                        inputs.emplace(std::move(declaration));
                    }
                    else
                    {
                        inputs.emplace(normalized);
                    }
                }
                for (auto const& resource : member(deployment, "resources").items())
                {
                    UF_TRY_VALUE(
                        normalized,
                        normalizeInputPath(
                            std::filesystem::path{member(resource, "path").string()}
                        )
                    );
                    inputs.emplace(std::move(normalized));
                }
            }

            auto declared = std::vector<std::string>{};
            declared.reserve(inputs.size());
            for (auto const& input : inputs)
                declared.emplace_back(input);
            return canonicalInputs(sourceDirectory, declared);
        }

        // One member of an object the published schema has already judged.
        //
        // Total by construction rather than by luck: every member read through
        // it is `required` in schema/umbraflow-project-v3.schema.json and of
        // the type stated there, so validate() has already refused every
        // document in which the lookup could fail. The check is here so that a
        // member removed from the schema without being removed here stops the
        // process at the contract instead of reading a neutral value and
        // building a different artifact. The loader spells the same helper the
        // same way (project-directory.cpp:236-249).
        [[nodiscard]]
        auto member(json::Value const& object, std::string_view name)
            -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        // One PixelRect member of a validated rect object. The schema states
        // the bound this cast could fail at -- 0..2^32-1 for an offset, 1 up
        // for an extent -- so a document that reaches here has an exact
        // integer in range, and the refusal is the contract's rather than a
        // reachable branch.
        [[nodiscard]]
        auto pixelEdge(json::Value const& rect, std::string_view name) -> uint32
        {
            auto const edge = checkedIntegralCast<uint32>(
                member(rect, name).number()
            );
            UF_CHECK(edge.has_value());
            return *edge;
        }

        [[nodiscard]]
        auto declaredTemplateCuts(
            json::Value const& document
        ) -> Result<std::vector<ProjectTemplateCutSpec>>
        {
            auto cuts = std::vector<ProjectTemplateCutSpec>{};
            auto const& declared = member(document, "template_cuts");
            cuts.reserve(declared.items().size());
            for (auto const& cut : declared.items())
            {
                auto hashes = std::vector<ContentHash>{};
                auto const& encoded = member(cut, "source_sha256s");
                hashes.reserve(encoded.items().size());
                for (auto const& source : encoded.items())
                {
                    UF_TRY_VALUE(
                        hash,
                        ContentHash::parse(
                            "sha256:" + std::string{source.string()}
                        )
                    );
                    hashes.emplace_back(hash);
                }

                auto const& rect = member(cut, "rect");
                UF_TRY_VALUE(
                    region,
                    PixelRect::create(
                        pixelEdge(rect, "x"),
                        pixelEdge(rect, "y"),
                        pixelEdge(rect, "width"),
                        pixelEdge(rect, "height")
                    )
                );
                cuts.emplace_back(ProjectTemplateCutSpec{
                    .templatePath = std::filesystem::path{
                        member(cut, "template").string()
                    },
                    .sourceHashes = std::move(hashes),
                    .rect         = region,
                });
            }
            return cuts;
        }

        [[nodiscard]]
        auto resourceKindOf(std::string_view kind)
            -> operator_runtime::ProjectResourceKind
        {
            if (kind == "json")
                return operator_runtime::ProjectResourceKind::Json;
            if (kind == "utf8")
                return operator_runtime::ProjectResourceKind::Utf8;
            if (kind == "bytes")
                return operator_runtime::ProjectResourceKind::Bytes;
            UF_UNREACHABLE_MSG("project schema admitted an unknown resource kind");
        }

        [[nodiscard]]
        auto declaredRegistrations(json::Value const& document)
            -> std::vector<ProjectRegistrationBuildSpec>
        {
            auto registrations = std::vector<ProjectRegistrationBuildSpec>{};
            for (auto const& deployment : member(document, "deployments").items())
            {
                auto closureOf = [&deployment](std::string_view slot)
                {
                    auto const& declared = member(deployment, slot);
                    auto closure = ProjectClosureBuildSpec{
                        .entryModule = std::string{
                            member(declared, "entry").string()
                        },
                    };
                    for (
                        auto const& entryPoint
                        : member(declared, "exported_entry_points").items()
                    )
                    {
                        closure.exportedEntryPoints.emplace_back(
                            entryPoint.string()
                        );
                    }
                    for (auto const& module : member(declared, "modules").items())
                    {
                        closure.modules.emplace_back(ProjectModuleSpec{
                            .name = std::string{member(module, "name").string()},
                            .sourceInput = std::filesystem::path{
                                member(module, "path").string()
                            },
                        });
                    }
                    return closure;
                };
                auto registration = ProjectRegistrationBuildSpec{
                    .deploymentName = std::string{member(deployment, "name").string()},
                    .pluginId       = std::string{member(deployment, "plugin_id").string()},
                    .toolClosure    = closureOf("tool_closure"),
                };
                for (auto const& resource : member(deployment, "resources").items())
                {
                    registration.resources.emplace_back(ProjectResourceSpec{
                        .kind = resourceKindOf(member(resource, "kind").string()),
                        .name = std::string{member(resource, "name").string()},
                        .sourceInput = std::filesystem::path{
                            member(resource, "path").string()
                        },
                    });
                }
                registrations.emplace_back(std::move(registration));
            }
            return registrations;
        }

        // Every executable declaration the offline kit acts on, extracted
        // from one schema-validated parse. No caller can restate a module or
        // resource closure beside the project's own root document.
        [[nodiscard]]
        auto readProjectManifest(
            std::filesystem::path const& sourceDirectory
        ) -> Result<ProjectManifest>
        {
            UF_TRY_VALUE(document, readProjectRootDocument(sourceDirectory));
            UF_TRY_VALUE(inputs, derivedInputs(sourceDirectory, document));
            UF_TRY_VALUE(templateCuts, declaredTemplateCuts(document));
            return ProjectManifest{
                .inputs        = std::move(inputs),
                .templateCuts  = std::move(templateCuts),
                .registrations = declaredRegistrations(document),
            };
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
                // The adapter directory is the declaration's own name and
                // the closure is a file inside it, so the staged module path
                // says which declaration it came from.
                auto const adapterDirectory =
                    std::filesystem::path{pluginId} / components.back().stem();
                adapters.emplace_back(
                    GeneratedArtifact{
                        .relativePath = adapterDirectory
                            / (std::string{k_toolClosureName} + ".luau"),
                        .bytes = std::move(adapter.toolModule),
                    }
                );
            }
            return adapters;
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
                    // The hash is in the message rather than in the error's
                    // context because the `project` command line prints
                    // message() and nothing else, and "the refusal names the
                    // source it could not resolve" has to be this function's
                    // property rather than each caller's. The resolver's own
                    // reason is quoted whole, so a caller that can say how to
                    // supply the missing bytes still gets to say it.
                    auto resolved = resolveTemplateSource(sourceHash);
                    if (!resolved)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "generated template \"{}\" cannot resolve "
                                "source {}: {}",
                                templatePath.generic_string(),
                                sourceHash.hex(),
                                resolved.error().message()
                            )
                        );
                    }
                    auto const encoded = *std::move(resolved);
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

        // What staging one closure produced: the rows the execution-closure
        // record states for it, the module blobs staged under the build tree,
        // and the digest that closure is identified by.
        //
        // ContentHash has no default state, so this struct has none either.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
        struct StagedClosure final
        {
            std::vector<json::Value>       moduleRows{};
            std::vector<GeneratedArtifact> stagedModules{};
            ContentHash                    moduleManifestHash;
        };

        // Stages one closure's modules and derives its manifest digest.
        //
        // `closureName` is the slot the closure fills, and it is part of the
        // staged path rather than decoration: both closures of a deployment
        // may carry a logical module of the same name, and a path without the
        // slot would let one closure's bytes overwrite the other's.
        [[nodiscard]]
        auto stagedClosure(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs,
            std::vector<GeneratedArtifact> const& generatedAdapters,
            std::string_view deploymentName,
            std::string_view closureName,
            ProjectClosureBuildSpec const& closure
        ) -> Result<StagedClosure>
        {
            auto normalizedModules = closure.modules;
            for (auto& module : normalizedModules)
            {
                UF_TRY_VALUE(normalized, normalizeInputPath(module.sourceInput));
                module.sourceInput = std::filesystem::path{normalized};
            }
            std::ranges::sort(normalizedModules, {}, &ProjectModuleSpec::name);

            auto modulePaths = std::set<std::string>{};
            auto moduleBlobs = std::vector<operator_runtime::ProjectModuleBlob>{};
            auto moduleRows  = std::vector<json::Value>{};
            auto stagedBlobs = std::vector<GeneratedArtifact>{};
            moduleBlobs.reserve(normalizedModules.size());
            moduleRows.reserve(normalizedModules.size());
            stagedBlobs.reserve(normalizedModules.size());
            for (auto const& module : normalizedModules)
            {
                auto const sourcePath = module.sourceInput.generic_string();
                if (!modulePaths.emplace(sourcePath).second)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project deployment {} uses module path \"{}\" "
                            "more than once in its {} closure",
                            deploymentName,
                            sourcePath,
                            closureName
                        )
                    );
                }

                auto bytes = std::string{};
                constexpr auto generatedPrefix = std::string_view{"generated/adapters/"};
                if (std::string_view{sourcePath}.starts_with(generatedPrefix))
                {
                    auto const relative = std::string_view{sourcePath}.substr(
                        generatedPrefix.size()
                    );
                    auto const generated = std::ranges::find_if(
                        generatedAdapters,
                        [relative](GeneratedArtifact const& artifact)
                        {
                            return artifact.relativePath.generic_string() == relative;
                        }
                    );
                    if (generated == generatedAdapters.end())
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "project module {} names missing generated adapter \"{}\"",
                                module.name,
                                sourcePath
                            )
                        );
                    }
                    bytes = generated->bytes;
                }
                else
                {
                    // Derived from this same declaration, so a module path is
                    // in the input set by construction rather than by two
                    // lists agreeing. It was a refusal while the set was a
                    // ledger a separate command wrote and the two could drift;
                    // with the ledger gone the refusal could not fire, and a
                    // check that cannot fail is worse than none -- see
                    // docs/pitfalls/checks-that-cannot-fail.md. What CAN fail
                    // is the file being absent, which validateDeclaredInput
                    // refuses by name before anything reaches here.
                    UF_CHECK(std::ranges::binary_search(inputs, sourcePath));
                    UF_TRY_VALUE(
                        sourceBytes,
                        readText(sourceDirectory / module.sourceInput, "project module source")
                    );
                    bytes = std::move(sourceBytes);
                }
                UF_TRY_VALUE(digest, sha256(std::as_bytes(std::span{bytes})));
                auto const storedPath = (
                    std::filesystem::path{deploymentName}
                    / closureName
                    / (module.name + ".luau")
                );
                moduleRows.emplace_back(json::Value::ofObject({
                    {"name", json::Value::ofString(module.name)},
                    {"path", json::Value::ofString(
                        (std::filesystem::path{k_generatedDirectory}
                         / k_generatedModuleDirectory
                         / storedPath).generic_string()
                    )},
                    {"sha256", json::Value::ofString(digest.hex())},
                    {"size", json::Value::ofNumber(static_cast<double>(bytes.size()))},
                }));
                moduleBlobs.emplace_back(
                    operator_runtime::ProjectModuleBlob{
                        .name   = module.name,
                        .source = bytes,
                    }
                );
                stagedBlobs.emplace_back(GeneratedArtifact{
                    .relativePath = storedPath,
                    .bytes        = std::move(bytes),
                });
            }
            UF_TRY_VALUE(
                moduleManifestHash,
                operator_runtime::derivePluginModuleManifestHash(
                    closure.entryModule,
                    moduleBlobs
                )
            );
            return StagedClosure{
                .moduleRows         = std::move(moduleRows),
                .stagedModules      = std::move(stagedBlobs),
                .moduleManifestHash = moduleManifestHash,
            };
        }

        [[nodiscard]]
        auto closureRecord(
            ProjectClosureBuildSpec const& closure,
            StagedClosure staged
        ) -> json::Value
        {
            auto entryPoints = std::vector<json::Value>{};
            entryPoints.reserve(closure.exportedEntryPoints.size());
            for (auto const& entryPoint : closure.exportedEntryPoints)
            {
                entryPoints.emplace_back(json::Value::ofString(entryPoint));
            }
            return json::Value::ofObject({
                {"entry", json::Value::ofString(closure.entryModule)},
                {"exported_entry_points",
                 json::Value::ofArray(std::move(entryPoints))},
                {"module_manifest_hash",
                 json::Value::ofString(staged.moduleManifestHash.hex())},
                {"modules", json::Value::ofArray(std::move(staged.moduleRows))},
            });
        }

        [[nodiscard]]
        auto generatedExecutionClosures(
            std::filesystem::path const& sourceDirectory,
            std::vector<std::string> const& inputs,
            std::vector<GeneratedArtifact> const& generatedAdapters,
            std::vector<ProjectRegistrationBuildSpec> const& registrations
        ) -> Result<std::vector<GeneratedArtifactFamily>>
        {
            auto orderedRegistrations = registrations;
            std::ranges::sort(
                orderedRegistrations,
                {},
                &ProjectRegistrationBuildSpec::deploymentName
            );
            for (auto index = std::size_t{1U}; index < orderedRegistrations.size(); ++index)
            {
                if (
                    orderedRegistrations[index - 1U].deploymentName
                    == orderedRegistrations[index].deploymentName
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project deployment {} appears more than once",
                            orderedRegistrations[index].deploymentName
                        )
                    );
                }
            }

            UF_TRY_VALUE(
                environmentHash,
                operator_runtime::currentProjectPluginEnvironmentHash()
            );
            auto modules        = std::vector<GeneratedArtifact>{};
            auto resources      = std::vector<GeneratedArtifact>{};
            auto closureRecords = std::vector<GeneratedArtifact>{};

            for (auto& registration : orderedRegistrations)
            {
                UF_TRY_VALUE(
                    toolClosure,
                    stagedClosure(
                        sourceDirectory,
                        inputs,
                        generatedAdapters,
                        registration.deploymentName,
                        k_toolClosureName,
                        registration.toolClosure
                    )
                );
                for (auto& staged : toolClosure.stagedModules)
                {
                    modules.emplace_back(std::move(staged));
                }

                auto normalizedResources = registration.resources;
                for (auto& resource : normalizedResources)
                {
                    UF_TRY_VALUE(normalized, normalizeInputPath(resource.sourceInput));
                    if (!std::ranges::binary_search(inputs, normalized))
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "project resource {} names undeclared source input \"{}\"",
                                resource.name,
                                normalized
                            )
                        );
                    }
                    resource.sourceInput = std::filesystem::path{normalized};
                }
                std::ranges::sort(normalizedResources, {}, &ProjectResourceSpec::name);
                auto resourcePaths = std::set<std::string>{};
                auto resourceNames = std::set<std::string>{};
                auto resourceBlobs = std::vector<
                    operator_runtime::ProjectResourceBlob
                >{};
                resourceBlobs.reserve(normalizedResources.size());
                for (auto const& resource : normalizedResources)
                {
                    if (!resourceNames.emplace(resource.name).second)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "project deployment {} names resource {} more than once",
                                registration.deploymentName,
                                resource.name
                            )
                        );
                    }
                    auto const sourcePath = resource.sourceInput.generic_string();
                    if (!resourcePaths.emplace(sourcePath).second)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "project deployment {} uses resource path \"{}\" more than once",
                                registration.deploymentName,
                                sourcePath
                            )
                        );
                    }
                    UF_TRY_VALUE(
                        bytes,
                        readText(sourceDirectory / resource.sourceInput, "project resource source")
                    );
                    resourceBlobs.emplace_back(
                        operator_runtime::ProjectResourceBlob{
                            .kind  = resource.kind,
                            .name  = resource.name,
                            .bytes = std::move(bytes),
                        }
                    );
                }
                UF_TRY_CONTEXT(
                    operator_runtime::validateProjectResourceClosure(resourceBlobs),
                    std::format(
                        "validating project resource closure for deployment {}",
                        registration.deploymentName
                    )
                );

                auto resourceRows = std::vector<json::Value>{};
                resourceRows.reserve(resourceBlobs.size());
                for (auto& resource : resourceBlobs)
                {
                    UF_TRY_VALUE(
                        digest,
                        sha256(std::as_bytes(std::span{resource.bytes}))
                    );
                    auto kind = std::string{"json"};
                    if (resource.kind == operator_runtime::ProjectResourceKind::Utf8)
                        kind = "utf8";
                    else if (resource.kind == operator_runtime::ProjectResourceKind::Bytes)
                        kind = "bytes";
                    auto const storedPath = (
                        std::filesystem::path{registration.deploymentName}
                        / (resource.name + ".blob")
                    );
                    resourceRows.emplace_back(json::Value::ofObject({
                        {"kind", json::Value::ofString(std::move(kind))},
                        {"name", json::Value::ofString(resource.name)},
                        {"path", json::Value::ofString(
                            (std::filesystem::path{k_generatedDirectory}
                             / k_generatedResourceDirectory
                             / storedPath).generic_string()
                        )},
                        {"sha256", json::Value::ofString(digest.hex())},
                        {"size", json::Value::ofNumber(
                            static_cast<double>(resource.bytes.size())
                        )},
                    }));
                    resources.emplace_back(GeneratedArtifact{
                        .relativePath = storedPath,
                        .bytes        = std::move(resource.bytes),
                    });
                }

                closureRecords.emplace_back(GeneratedArtifact{
                    .relativePath = registration.deploymentName + ".json",
                    .bytes = json::canonicalBytes(json::Value::ofObject({
                        {"plugin_environment_hash", json::Value::ofString(environmentHash.hex())},
                        {"plugin_id", json::Value::ofString(registration.pluginId)},
                        {"project_resources", json::Value::ofArray(std::move(resourceRows))},
                        {"schema", json::Value::ofString(std::string{k_executionClosureSchema})},
                        {"tool_closure", closureRecord(
                            registration.toolClosure,
                            std::move(toolClosure)
                        )},
                    })),
                });
            }

            return std::vector<GeneratedArtifactFamily>{
                GeneratedArtifactFamily{
                    .directory = std::string{k_generatedModuleDirectory},
                    .artifacts = std::move(modules),
                },
                GeneratedArtifactFamily{
                    .directory = std::string{k_generatedResourceDirectory},
                    .artifacts = std::move(resources),
                },
                GeneratedArtifactFamily{
                    .directory = std::string{k_generatedRegistrationDirectory},
                    .artifacts = std::move(closureRecords),
                },
            };
        }

        [[nodiscard]]
        auto generatedProjectBuild(
            ProjectBuildSpec const& spec,
            std::vector<std::string> const& inputs,
            std::vector<ProjectTemplateCutSpec> const& templateCuts,
            std::vector<ProjectRegistrationBuildSpec> const& registrations,
            TemplateSourceResolver const& resolveTemplateSource
        ) -> Result<GeneratedProjectBuild>
        {
            UF_TRY_VALUE(
                adapters,
                generatedAdapters(spec.sourceDirectory, inputs)
            );
            UF_TRY_VALUE(
                templates,
                generatedTemplates(templateCuts, resolveTemplateSource)
            );
            UF_TRY_VALUE(
                closureFamilies,
                generatedExecutionClosures(
                    spec.sourceDirectory,
                    inputs,
                    adapters,
                    registrations
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
        auto confinedGeneratedArtifactPath(
            std::filesystem::path const& artifactRoot,
            std::filesystem::path const& relativePath
        ) -> Result<std::filesystem::path>
        {
            auto const normalized = relativePath.lexically_normal();
            if (
                normalized.empty()
                || normalized == std::filesystem::path{"."}
                || normalized != relativePath
                || normalized.is_absolute()
                || normalized.has_root_name()
                || normalized.has_root_directory()
                || std::ranges::any_of(
                    normalized,
                    [](std::filesystem::path const& component)
                    {
                        return component == std::filesystem::path{".."};
                    }
                )
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "generated artifact path must be canonical and relative: \"{}\"",
                        relativePath.string()
                    )
                );
            }
            auto const path = artifactRoot / normalized;
            if (!isWithinOrEqual(path, artifactRoot))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "generated artifact path leaves its artifact family"
                );
            }
            return path;
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
                UF_TRY_VALUE(
                    path,
                    confinedGeneratedArtifactPath(artifactRoot, artifact.relativePath)
                );
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
                UF_TRY(confinedGeneratedArtifactPath(
                    artifactRoot,
                    artifact.relativePath
                ));
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
                UF_TRY_VALUE(
                    path,
                    confinedGeneratedArtifactPath(artifactRoot, artifact.relativePath)
                );
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

        // The rules about a deployment that no JSON Schema can state, because
        // each one compares two members of the document with each other.
        //
        // They are applied HERE, on the read every extraction follows, rather
        // than in build or in check, because the defect they close is a reader
        // knowing less than a later reader: `project check` passed a real
        // migration and `umbra-flow open` then refused the same directory with
        // `Tool name chaos.get_current_event is outside the namespace
        // chaos.dream its registrant owns`. An author working through a
        // declaration must not be told twice, in two commands, hours apart.
        //
        // Neither rule is restated here. Both are the Operator's own functions,
        // called by the loader on a verified registration and by this kit on
        // the declaration that produces one, so the two readers cannot reach
        // two verdicts.
        [[nodiscard]]
        auto validateDeploymentJoins(json::Value const& document) -> Status
        {
            for (auto const& deployment : member(document, "deployments").items())
            {
                auto const name     = member(deployment, "name").string();
                auto const pluginId = member(deployment, "plugin_id").string();

                // Which deployment, in the message rather than in an error
                // context nothing on this path prints -- and worded the way
                // the loader words the same failure, so an author who meets it
                // twice meets one sentence.
                auto const inDeployment = [name](Status outcome) -> Status
                {
                    if (outcome.has_value())
                    {
                        return ok();
                    }
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "the deployment {} does not hold together: {}",
                            name,
                            outcome.error().message()
                        )
                    );
                };

                auto toolNames = std::vector<std::string>{};
                for (auto const& tool : member(deployment, "tools").items())
                {
                    auto const toolName = member(tool, "name").string();
                    UF_TRY(inDeployment(
                        operator_runtime::validateToolNameOwnership(
                            toolName,
                            pluginId
                        )
                    ));
                    toolNames.emplace_back(toolName);
                }

                auto bindings = std::vector<operator_runtime::ProjectToolBinding>{};
                for (
                    auto const& binding
                    : member(deployment, "tool_bindings").items()
                )
                {
                    bindings.emplace_back(operator_runtime::ProjectToolBinding{
                        .toolName = std::string{
                            member(binding, "tool_name").string()
                        },
                        .entryPoint = std::string{
                            member(binding, "entry_point").string()
                        },
                    });
                }

                auto entryPoints = std::vector<std::string>{};
                auto const& closure = member(deployment, "tool_closure");
                for (
                    auto const& entryPoint
                    : member(closure, "exported_entry_points").items()
                )
                {
                    entryPoints.emplace_back(entryPoint.string());
                }

                UF_TRY(inDeployment(
                    operator_runtime::validateProjectToolBindings(
                        toolNames,
                        bindings,
                        entryPoints
                    )
                ));
            }
            return ok();
        }

        [[nodiscard]]
        auto validatedProjectDocument(
            std::string_view bytes
        ) -> Result<json::Value>
        {
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
                // The verdict, then the work list. validate stays the only
                // authority on whether this document is accepted; the account
                // below never accepts anything and runs only because the
                // verdict was already a refusal. It is attached to the refusal
                // rather than printed separately so that every caller of this
                // function -- build, check, the scaffold's own self-check, and
                // the report an upgrade runs -- shows it without arranging to.
                // An empty account is one with nothing to add to the refusal,
                // which is most of them.
                auto const account = compiled->explainRefusal(
                    document,
                    judged.error()
                );
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{}: {}{}{}",
                        k_projectManifestName,
                        judged.error().message(),
                        account.empty() ? "" : "\n\n",
                        account
                    )
                );
            }
            UF_TRY(validateDeploymentJoins(document));
            return document;
        }
    }

    // The project's root document, judged by the one published statement of
    // its shape before any reader pulls a member out of it.
    //
    // It is read from the source tree unconditionally. The declared-input
    // list is the author's, so a gate that ran only when the author had
    // listed umbraflow-project.json was one the author could switch off by
    // not listing it, while the tree still held the document the runtime
    // loader would later read.
    //
    // Reading a member out of the value validate() just accepted is not a
    // second reading of this document: it is the same parse, and the shape it
    // is allowed to have was decided by the published schema and nowhere
    // else. What would be a second reading -- and is the defect this file
    // already carries the scar of -- is any caller opening this file to pull
    // members out before the schema has judged them. This function is the
    // read every extraction is allowed to follow: template_cuts in
    // readProjectManifest, and the deployment declarations in command.cpp.
    //
    // schema/umbraflow-project-v3.schema.json
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
    auto readProjectRootDocument(
        std::filesystem::path const& sourceDirectory
    ) -> Result<json::Value>
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

        return validatedProjectDocument(bytes);
    }

    auto scaffoldProject(ProjectScaffoldSpec const& spec) -> Status
    {
        UF_TRY(requireDirectory(spec.sourceDirectory, "source"));

        // A starter authors one JSON document and one Luau closure. It writes
        // no JSON Schema at all: the Tool it declares carries its argument
        // shape inline, and a project that observes nothing needs no identity
        // schema, so there is nothing left for a separate file to hold.
        auto const projectDocument = scaffoldProjectDocument(spec);
        auto const projectBytes    = jsonDocument(projectDocument);
        UF_TRY(validatedProjectDocument(projectBytes));

        // The genesis RuntimeArtifact, written at initialisation so a starter
        // project HAS a model to pin from its first session. It declares no
        // ui target, no binding and no surface, so the session it admits can
        // do nothing -- which is the honest state of a project nobody has
        // annotated yet, and the root of its parentage chain. The bytes are
        // the framework's constant rather than this file's: every project in
        // the universe starts from the same H_genesis.
        auto const artifactDirectory =
            std::filesystem::path{"runtime"} / "artifact";
        UF_TRY_VALUE(genesisManifest, task::genesisRuntimeArtifactManifestJcs());

        auto files = std::vector<ScaffoldFile>{
            {
                .relativePath = "content/placeholder.txt",
                .bytes        = "replace this runtime resource\n",
            },
            {
                .relativePath = artifactDirectory
                    / std::string{task::k_runtimeModelFileName},
                .bytes = std::string{task::k_genesisRuntimeModelToml},
            },
            {
                .relativePath = artifactDirectory
                    / std::string{task::k_runtimeArtifactManifestFileName},
                .bytes = std::move(genesisManifest),
            },
        };

        switch (spec.pluginForm)
        {
        case ProjectPluginForm::Generated:
        {
            auto const declaration = scaffoldDeclarativeTool(spec.pluginId);
            UF_TRY(generateDeclarativeWorkflowAdapter(
                spec.pluginId,
                jsonDocument(declaration)
            ));
            files.emplace_back(ScaffoldFile{
                .relativePath = std::filesystem::path{
                    "declarative-tools"
                } / spec.pluginId / "scaffold.json",
                .bytes = jsonDocument(declaration),
            });
            break;
        }
        case ProjectPluginForm::HandWritten:
            files.emplace_back(ScaffoldFile{
                .relativePath = "plugin/support.luau",
                .bytes = (
                    "return {\n"
                    "    identity = function(value)\n"
                    "        return value\n"
                    "    end,\n"
                    "}\n"
                ),
            });
            files.emplace_back(ScaffoldFile{
                .relativePath = "plugin/tool.luau",
                .bytes = (
                    "-- This deployment binds no Tool yet, so this closure\n"
                    "-- exports its identity and nothing else. Add an entry\n"
                    "-- here and name it in tool_bindings together.\n"
                    "local _support = require(\"./support\")\n\n"
                    "return {\n"
                    "    plugin_id = \"" + spec.pluginId + "\",\n"
                    "}\n"
                ),
            });
            break;
        }
        files.emplace_back(ScaffoldFile{
            .relativePath = std::filesystem::path{k_projectManifestName},
            .bytes        = projectBytes,
        });

        UF_TRY_VALUE(sourceRoot, resolvedPath(spec.sourceDirectory, "source"));
        for (auto const& file : files)
        {
            auto const target = spec.sourceDirectory / file.relativePath;
            UF_TRY_VALUE(resolvedTarget, resolvedPath(target, "scaffold target"));
            if (!isWithinOrEqual(resolvedTarget, sourceRoot))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "starter Project file leaves the source tree: \"{}\"",
                        file.relativePath.generic_string()
                    )
                );
            }

            auto error        = std::error_code{};
            auto const status = std::filesystem::symlink_status(target, error);
            if (
                error
                && error != std::errc::no_such_file_or_directory
            )
            {
                return fail(
                    AutomationErrorKind::IoFailure,
                    std::format(
                        "cannot inspect starter Project file \"{}\": {}",
                        target.string(),
                        error.message()
                    )
                );
            }
            if (
                !error
                && status.type() != std::filesystem::file_type::not_found
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "starter Project file already exists: \"{}\"",
                        target.string()
                    )
                );
            }
        }

        for (auto const& file : files)
        {
            UF_TRY(writeProjectFileIfMissing(
                spec.sourceDirectory / file.relativePath,
                file.bytes
            ));
        }
        return ok();
    }

    // Makes a directory a project: judges what is there, and gives the build
    // tree somewhere to land.
    //
    // It writes no input ledger, because there is none to write. The set of
    // source files a project has is arithmetic over its declaration, and a copy
    // of that arithmetic on disk could only ever be right until the next edit.
    // It also acquires nothing: installing a framework release is `project
    // upgrade`, which is destructive and says so in its name.
    auto initProject(ProjectBuildSpec const& spec) -> Status
    {
        UF_TRY(validateDirectories(spec));
        UF_TRY(readProjectManifest(spec.sourceDirectory));
        return ensureBuildDirectory(spec.buildDirectory);
    }

    auto buildProject(
        ProjectBuildSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Status
    {
        UF_TRY(validateDirectories(spec));
        UF_TRY_VALUE(projectManifest, readProjectManifest(spec.sourceDirectory));
        auto const& inputs = projectManifest.inputs;
        UF_TRY_VALUE(
            generated,
            generatedProjectBuild(
                spec,
                inputs,
                projectManifest.templateCuts,
                projectManifest.registrations,
                resolveTemplateSource
            )
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
            artifactManifest,
            createArtifactManifest(spec.sourceDirectory, inputs, generated)
        );
        return writeText(
            spec.buildDirectory / k_artifactManifestName,
            renderArtifactManifest(artifactManifest),
            "artifact manifest"
        );
    }

    // The verb an author re-runs after every edit.
    //
    // Everything it needs comes out of the source tree: the declaration, the
    // files that declaration names, and the artifacts a build of it would
    // produce. It reaches no network and touches no installed release, so it is
    // the one command that answers "what is still wrong" for as long as it
    // takes to work through the answer.
    auto checkProject(
        ProjectBuildSpec const& spec,
        TemplateSourceResolver const& resolveTemplateSource
    ) -> Status
    {
        UF_TRY(validateDirectories(spec));
        UF_TRY_VALUE(projectManifest, readProjectManifest(spec.sourceDirectory));
        auto const& inputs = projectManifest.inputs;
        UF_TRY_VALUE(
            generated,
            generatedProjectBuild(
                spec,
                inputs,
                projectManifest.templateCuts,
                projectManifest.registrations,
                resolveTemplateSource
            )
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
            artifactManifest,
            createArtifactManifest(spec.sourceDirectory, inputs, generated)
        );
        auto const manifestPath = spec.buildDirectory / k_artifactManifestName;
        UF_TRY_VALUE(actual, readText(manifestPath, "artifact manifest"));
        if (actual != renderArtifactManifest(artifactManifest))
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
