#include "command.hpp"

#include "project-kit.hpp"

#include <core/error/contracts.hpp>
#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace uf::project
{
    namespace
    {
        enum class ProjectFlag : uint8
        {
            Source,
            Build,
            Input,
            Release,
            FramesRoot,
            Plugin,
            PluginId,
        };

        struct ProjectFlagDefinition final
        {
            std::string_view name{};
            ProjectFlag      flag{};
        };

        struct ParsedProjectFlags final
        {
            std::optional<std::filesystem::path> source{};
            std::optional<std::filesystem::path> build{};
            std::optional<std::filesystem::path> release{};
            std::optional<std::filesystem::path> framesRoot{};
            std::vector<std::filesystem::path>   inputs{};
            std::optional<std::string>           plugin{};
            std::optional<std::string>           pluginId{};
        };

        constexpr auto k_projectFlags = std::array{
            ProjectFlagDefinition{"--source", ProjectFlag::Source},
            ProjectFlagDefinition{"--build", ProjectFlag::Build},
            ProjectFlagDefinition{"--input", ProjectFlag::Input},
            ProjectFlagDefinition{"--release", ProjectFlag::Release},
            ProjectFlagDefinition{"--frames-root", ProjectFlag::FramesRoot},
            ProjectFlagDefinition{"--plugin", ProjectFlag::Plugin},
            ProjectFlagDefinition{"--plugin-id", ProjectFlag::PluginId},
        };

        [[nodiscard]]
        auto parseProjectFlags(
            std::span<std::string const> raw
        ) -> Result<ParsedProjectFlags>
        {
            auto parsed = ParsedProjectFlags{};
            for (auto index = std::size_t{0}; index < raw.size(); index += 2U)
            {
                auto const definition = std::ranges::find(
                    k_projectFlags,
                    raw[index],
                    &ProjectFlagDefinition::name
                );
                if (definition == k_projectFlags.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "unknown project argument \"{}\"",
                            raw[index]
                        )
                    );
                }
                if (index + 1U == raw.size())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "project argument \"{}\" requires a value",
                            raw[index]
                        )
                    );
                }

                auto const& value = raw[index + 1U];
                switch (definition->flag)
                {
                case ProjectFlag::Source:
                    if (parsed.source)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--source\" appears more than once"
                        );
                    }
                    parsed.source = std::filesystem::path{value};
                    break;
                case ProjectFlag::Build:
                    if (parsed.build)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--build\" appears more than once"
                        );
                    }
                    parsed.build = std::filesystem::path{value};
                    break;
                case ProjectFlag::Input:
                    parsed.inputs.emplace_back(value);
                    break;
                case ProjectFlag::Release:
                    if (parsed.release)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--release\" appears more than once"
                        );
                    }
                    parsed.release = std::filesystem::path{value};
                    break;
                case ProjectFlag::FramesRoot:
                    if (parsed.framesRoot)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--frames-root\" appears more "
                            "than once"
                        );
                    }
                    parsed.framesRoot = std::filesystem::path{value};
                    break;
                case ProjectFlag::Plugin:
                    if (parsed.plugin)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--plugin\" appears more than once"
                        );
                    }
                    parsed.plugin = value;
                    break;
                case ProjectFlag::PluginId:
                    if (parsed.pluginId)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--plugin-id\" appears more than once"
                        );
                    }
                    parsed.pluginId = value;
                    break;
                }
            }
            return parsed;
        }

        [[nodiscard]]
        auto projectDirectories(
            ParsedProjectFlags const& parsed
        ) -> Result<ProjectDirectories>
        {
            auto source = std::filesystem::path{};
            if (parsed.source)
            {
                source = *parsed.source;
            }
            else
            {
                auto error = std::error_code{};
                source     = std::filesystem::current_path(error);
                if (error)
                {
                    return fail(
                        AutomationErrorKind::IoFailure,
                        std::format(
                            "cannot resolve the current project source "
                            "directory: {}",
                            error.message()
                        )
                    );
                }
            }
            auto const build = parsed.build
                ? *parsed.build
                : source / "work" / "build";
            return ProjectDirectories{
                .sourceDirectory = std::move(source),
                .buildDirectory  = build,
            };
        }

        // The corpus one template source is resolved in, and the only place
        // this program learns what a directory is on the kit's behalf.
        //
        // The store is named by content: the bytes of hash H live in H.png, so
        // a hash opens a file with no index between them and no path written
        // into the project. That is what makes "no screenshot references"
        // structural: there is nothing a project could write down that would
        // pin a source to one machine's filesystem.
        //
        // WHY A FLAG AND NOT AN ENVIRONMENT VARIABLE. Nothing in this
        // repository reads the environment -- the Luau sandbox goes as far as
        // removing os.getenv (tests/script/test-veto-suite.cpp) -- and a build
        // whose output depends on ambient state produces two different
        // artifacts from one command line with nothing recording which. The
        // corpus root is on the command line, so the invocation that produced
        // a release is the whole account of what produced it.
        //
        // Nothing here verifies the bytes. generatedTemplates re-hashes every
        // answer and refuses one that does not hash to what it asked for, so a
        // store whose file names lie is caught in one place; verifying here as
        // well would be a second spelling of that rule, and the kit's would
        // stop being the one that fires.
        [[nodiscard]]
        auto templateSourceResolver(
            std::optional<std::filesystem::path> framesRoot
        ) -> TemplateSourceResolver
        {
            // By value, and no reference to anything the caller owns: the
            // resolver outlives this frame and is called from inside the kit.
            return [root = std::move(framesRoot)](
                       ContentHash const& requested
                   ) -> Result<std::vector<std::byte>>
            {
                auto const name = requested.hex() + ".png";
                // A machine with no corpus still gets a resolver rather than
                // none. An empty std::function makes the kit refuse the
                // declaration without naming a hash, and a resolver that
                // answered "skip this one" would build a different artifact
                // out of the same source, which is worse than any refusal.
                if (!root)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "no template source corpus was given: pass "
                            "--frames-root PATH naming a directory that holds "
                            "\"{}\"",
                            name
                        )
                    );
                }

                auto const path = *root / name;
                auto stream     = std::ifstream{path, std::ios::binary};
                if (!stream.is_open())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format(
                            "template source corpus \"{}\" holds no \"{}\"",
                            root->string(),
                            name
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
                            "cannot read template source \"{}\"",
                            path.string()
                        )
                    );
                }
                auto const bytes = std::as_bytes(std::span{text});
                return std::vector<std::byte>{bytes.begin(), bytes.end()};
            };
        }

        // One member of an object the published project schema has already
        // judged. Total by construction, the same way the kit's helper is
        // (project-kit.cpp): every member read through it is required in
        // schema/umbraflow-project-v2.schema.json and of the type stated
        // there, so the extraction below happens only on the value
        // readProjectRootDocument let the schema judge.
        [[nodiscard]]
        auto member(json::Value const& object, std::string_view name)
            -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        // One declared tool catalog source: the path a deployment declaration
        // names, resolved against the source tree the way the runtime loader
        // resolves every member it reads, and read back into the declaration
        // the generator renders. A declared source the tree does not hold is
        // refused by name -- the same rule a declared cut's missing source
        // obeys -- and never quietly skipped.
        [[nodiscard]]
        auto declaredToolCatalog(
            std::filesystem::path const& sourceDirectory,
            json::Value const& deployment
        ) -> Result<ToolCatalogDeclaration>
        {
            auto const declaredPath = std::filesystem::path{
                member(deployment, "tool_catalog").string()
            };
            auto const path = sourceDirectory / declaredPath;
            auto stream     = std::ifstream{path, std::ios::binary};
            if (!stream.is_open())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "the deployment declaration names tool catalog source "
                        "\"{}\", which \"{}\" does not hold",
                        declaredPath.string(),
                        sourceDirectory.string()
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
                        "cannot read declared tool catalog source \"{}\"",
                        path.string()
                    )
                );
            }
            UF_TRY_VALUE_CONTEXT(
                document,
                json::parse(text),
                std::format(
                    "reading declared tool catalog source \"{}\"",
                    path.string()
                )
            );
            UF_TRY_VALUE_CONTEXT(
                declaration,
                parseToolCatalogDeclaration(document),
                std::format(
                    "reading declared tool catalog source \"{}\"",
                    path.string()
                )
            );
            return declaration;
        }

        // The caller supplies only the semantic Tool Catalog declarations it
        // has parsed. Module and resource closures stay inside the kit and are
        // derived from the already-validated root manifest; exposing them here
        // would create a second, caller-controlled spelling of one project.
        [[nodiscard]]
        auto attachDeploymentDeclarations(ProjectBuildSpec& spec) -> Status
        {
            UF_TRY_VALUE(document, readProjectRootDocument(spec.sourceDirectory));
            auto toolCatalogs = std::vector<ToolCatalogDeclaration>{};
            for (auto const& deployment : member(document, "deployments").items())
            {
                UF_TRY_VALUE(
                    catalog,
                    declaredToolCatalog(spec.sourceDirectory, deployment)
                );
                toolCatalogs.emplace_back(std::move(catalog));

            }
            spec.toolCatalogs = std::move(toolCatalogs);
            return ok();
        }

        struct ParsedProjectInit final
        {
            ProjectInitSpec                    spec{};
            std::optional<ProjectScaffoldSpec> scaffold{};
        };

        struct ProjectPluginFormDefinition final
        {
            std::string_view  name{};
            ProjectPluginForm form{};
        };

        constexpr auto k_projectPluginForms = std::array{
            ProjectPluginFormDefinition{
                "generated",
                ProjectPluginForm::Generated,
            },
            ProjectPluginFormDefinition{
                "hand-written",
                ProjectPluginForm::HandWritten,
            },
        };

        [[nodiscard]]
        auto parseProjectInit(
            std::span<std::string const> raw
        ) -> Result<ParsedProjectInit>
        {
            UF_TRY_VALUE(parsed, parseProjectFlags(raw));
            if (parsed.release || parsed.framesRoot)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project init does not accept --release or --frames-root"
                );
            }
            if (parsed.plugin.has_value() != parsed.pluginId.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "a starter Project requires --plugin and --plugin-id together"
                );
            }
            UF_TRY_VALUE(directories, projectDirectories(parsed));

            auto scaffold = std::optional<ProjectScaffoldSpec>{};
            if (parsed.plugin)
            {
                auto const form = std::ranges::find(
                    k_projectPluginForms,
                    *parsed.plugin,
                    &ProjectPluginFormDefinition::name
                );
                if (form == k_projectPluginForms.end())
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "project --plugin must be generated or hand-written"
                    );
                }
                scaffold.emplace(ProjectScaffoldSpec{
                    .sourceDirectory = directories.sourceDirectory,
                    .pluginId        = std::move(*parsed.pluginId),
                    .pluginForm      = form->form,
                });
            }
            return ParsedProjectInit{
                .spec = ProjectInitSpec{
                    .sourceDirectory = std::move(directories.sourceDirectory),
                    .buildDirectory  = std::move(directories.buildDirectory),
                    .inputs          = std::move(parsed.inputs),
                },
                .scaffold = std::move(scaffold),
            };
        }

        // What one invocation of build, check or freeze decided: the spec the
        // kit is given, and the corpus root it is deliberately not given. The
        // root stays outside ProjectBuildSpec because the kit must never learn
        // what a directory is -- it reaches the kit only as the resolver's
        // captured state.
        struct ParsedProjectBuild final
        {
            ProjectBuildSpec                     spec{};
            std::optional<std::filesystem::path> framesRoot{};
        };

        struct ParsedProjectFreeze final
        {
            ProjectFreezeSpec                    spec{};
            std::optional<std::filesystem::path> framesRoot{};
        };

        [[nodiscard]]
        auto parseProjectBuildSpec(
            std::span<std::string const> raw,
            std::string_view action
        ) -> Result<ParsedProjectBuild>
        {
            UF_TRY_VALUE(parsed, parseProjectFlags(raw));
            if (
                !parsed.inputs.empty()
                || parsed.release
                || parsed.plugin
                || parsed.pluginId
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project {} accepts only --source, --build and "
                        "--frames-root",
                        action
                    )
                );
            }
            UF_TRY_VALUE(directories, projectDirectories(parsed));
            return ParsedProjectBuild{
                .spec = ProjectBuildSpec{
                    .sourceDirectory = std::move(directories.sourceDirectory),
                    .buildDirectory  = std::move(directories.buildDirectory),
                    .toolCatalogs    = {},
                },
                .framesRoot = std::move(parsed.framesRoot),
            };
        }

        [[nodiscard]]
        auto parseProjectFreeze(
            std::span<std::string const> raw
        ) -> Result<ParsedProjectFreeze>
        {
            UF_TRY_VALUE(parsed, parseProjectFlags(raw));
            if (
                !parsed.inputs.empty()
                || parsed.plugin
                || parsed.pluginId
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project freeze does not accept --input, --plugin or "
                    "--plugin-id"
                );
            }
            UF_TRY_VALUE(directories, projectDirectories(parsed));
            auto release = parsed.release
                ? std::move(*parsed.release)
                : directories.sourceDirectory / "work" / "release";
            return ParsedProjectFreeze{
                .spec = ProjectFreezeSpec{
                    .candidate = ProjectBuildSpec{
                        .sourceDirectory = std::move(directories.sourceDirectory),
                        .buildDirectory  = std::move(directories.buildDirectory),
                        .toolCatalogs    = {},
                    },
                    .releaseRoot = std::move(release),
                },
                .framesRoot = std::move(parsed.framesRoot),
            };
        }

        [[nodiscard]]
        auto parseProjectRun(
            std::span<std::string const> raw
        ) -> Result<std::filesystem::path>
        {
            UF_TRY_VALUE(parsed, parseProjectFlags(raw));
            if (
                !parsed.release
                || parsed.source
                || parsed.build
                || parsed.framesRoot
                || !parsed.inputs.empty()
                || parsed.plugin
                || parsed.pluginId
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project run requires only --release PATH"
                );
            }
            return std::move(*parsed.release);
        }

        [[nodiscard]]
        auto reportProjectError(Error const& error) -> ProjectExitCode
        {
            std::cerr << error.message() << '\n';
            return ProjectExitCode::Failure;
        }

        [[nodiscard]]
        auto runProjectInit(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto const parsed = parseProjectInit(raw);
            if (!parsed)
            {
                std::cerr << parsed.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            if (parsed->scaffold)
            {
                auto const scaffolded = scaffoldProject(*parsed->scaffold);
                if (!scaffolded)
                {
                    return reportProjectError(scaffolded.error());
                }
            }

            auto const initialized = initProject(parsed->spec);
            if (!initialized)
            {
                return reportProjectError(initialized.error());
            }
            std::cout << std::format(
                "project init: source=\"{}\" build=\"{}\"\n",
                parsed->spec.sourceDirectory.string(),
                parsed->spec.buildDirectory.string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectBuild(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto parsed = parseProjectBuildSpec(raw, "build");
            if (!parsed)
            {
                std::cerr << parsed.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const attached = attachDeploymentDeclarations(parsed->spec);
            if (!attached)
            {
                return reportProjectError(attached.error());
            }

            auto const built = buildProject(
                parsed->spec,
                templateSourceResolver(parsed->framesRoot)
            );
            if (!built)
            {
                return reportProjectError(built.error());
            }
            std::cout << std::format(
                "project build: build=\"{}\"\n",
                parsed->spec.buildDirectory.string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectCheck(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto parsed = parseProjectBuildSpec(raw, "check");
            if (!parsed)
            {
                std::cerr << parsed.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const attached = attachDeploymentDeclarations(parsed->spec);
            if (!attached)
            {
                return reportProjectError(attached.error());
            }

            auto const checked = checkProject(
                parsed->spec,
                templateSourceResolver(parsed->framesRoot)
            );
            if (!checked)
            {
                return reportProjectError(checked.error());
            }
            std::cout << std::format(
                "project check: source=\"{}\" build=\"{}\"\n",
                parsed->spec.sourceDirectory.string(),
                parsed->spec.buildDirectory.string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectFreeze(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto parsed = parseProjectFreeze(raw);
            if (!parsed)
            {
                std::cerr << parsed.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const attached = attachDeploymentDeclarations(parsed->spec.candidate);
            if (!attached)
            {
                return reportProjectError(attached.error());
            }

            auto const release = freezeProject(
                parsed->spec,
                templateSourceResolver(parsed->framesRoot)
            );
            if (!release)
            {
                return reportProjectError(release.error());
            }
            std::cout << std::format(
                "project freeze: release_id={} release=\"{}\"\n",
                release->filename().string(),
                release->string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectRun(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto const release = parseProjectRun(raw);
            if (!release)
            {
                std::cerr << release.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const loaded = loadProjectRelease(*release);
            if (!loaded)
            {
                return reportProjectError(loaded.error());
            }
            std::cout << std::format(
                "project run: release_id={}\n",
                release->filename().string()
            );
            return ProjectExitCode::Success;
        }

        using ProjectCommandHandler = ProjectExitCode (*)(
            std::span<std::string const>
        );

        struct ProjectCommand final
        {
            std::string_view      name{};
            ProjectCommandHandler handler{};
        };

        constexpr auto k_projectCommands = std::array{
            ProjectCommand{"init", &runProjectInit},
            ProjectCommand{"build", &runProjectBuild},
            ProjectCommand{"check", &runProjectCheck},
            ProjectCommand{"freeze", &runProjectFreeze},
            ProjectCommand{"run", &runProjectRun},
        };
    }

    auto parseProjectDirectories(
        std::span<std::string const> raw,
        std::string_view action
    ) -> Result<ProjectDirectories>
    {
        if (action == "init")
        {
            UF_TRY_VALUE(parsed, parseProjectInit(raw));
            return ProjectDirectories{
                .sourceDirectory = std::move(parsed.spec.sourceDirectory),
                .buildDirectory  = std::move(parsed.spec.buildDirectory),
            };
        }
        UF_TRY_VALUE(parsed, parseProjectBuildSpec(raw, action));
        return ProjectDirectories{
            .sourceDirectory = std::move(parsed.spec.sourceDirectory),
            .buildDirectory  = std::move(parsed.spec.buildDirectory),
        };
    }

    auto runProjectCommand(
        std::span<std::string const> raw
    ) -> ProjectExitCode
    {
        if (raw.empty())
        {
            std::cerr << projectUsageText();
            return ProjectExitCode::Failure;
        }

        auto const command = std::ranges::find(
            k_projectCommands,
            raw.front(),
            &ProjectCommand::name
        );
        if (command == k_projectCommands.end())
        {
            std::cerr << std::format(
                "unknown project action \"{}\"\n",
                raw.front()
            );
            std::cerr << projectUsageText();
            return ProjectExitCode::Failure;
        }
        return command->handler(raw.subspan(1));
    }

    auto projectUsageText() noexcept -> std::string_view
    {
        return
            "Usage:\n"
            "  project init [--source PATH] [--build PATH] "
            "[--plugin generated|hand-written --plugin-id NAME] "
            "[--input RELATIVE_PATH ...]\n"
            "  project build [--source PATH] [--build PATH] "
            "[--frames-root PATH]\n"
            "  project check [--source PATH] [--build PATH] "
            "[--frames-root PATH]\n"
            "  project freeze [--source PATH] [--build PATH] [--release PATH] "
            "[--frames-root PATH]\n"
            "  project run --release RELEASE_DIRECTORY\n"
            "\n"
            "init creates a starter Project when umbraflow-project.json is\n"
            "absent, then derives its ordinary inputs from that document.\n"
            "--plugin and --plugin-id are required together only for that first\n"
            "init; --input adds an authoring-generator source that the runtime\n"
            "declaration does not carry. Source defaults to the current\n"
            "directory, build to <source>/work/build and release to\n"
            "<source>/work/release. When umbraflow-kit.json is present, init\n"
            "installs and verifies its release under <source>/umbraflow-bin; a\n"
            "later init verifies that immutable local bundle without following\n"
            "a newer release. build materializes generated artifacts, check\n"
            "judges them, freeze publishes a content-addressed read-only\n"
            "release, and run accepts only a verified immutable release.\n"
            "A derived declarative-tools/PLUGIN_ID/NAME.json input generates\n"
            "generated/adapters/PLUGIN_ID/NAME.luau. The source directory must\n"
            "hold umbraflow-project.json after init; build\n"
            "and check judge it against the published project schema, which\n"
            "requires a plugin_justification of every deployment whose\n"
            "plugin_authoring is hand-written and refuses one from every\n"
            "deployment whose plugin_authoring is generated.\n"
            "\n"
            "Every deployment also names its declared tool catalog, closed\n"
            "module set and typed resources. build and check materialize the\n"
            "exact execution bytes under generated/modules/ and\n"
            "generated/resources/. generated/registrations/DEPLOYMENT.json\n"
            "records the module-manifest and running environment identities\n"
            "plus every resource kind, digest and size; project authors type no\n"
            "digest in umbraflow-project.json.\n"
            "\n"
            "build also records every other file a deployment declaration\n"
            "names -- the four project schemas, the two manifests and the\n"
            "journal payload schemas -- by digest, and check holds each against\n"
            "that record: a declared file the tree does not hold, or whose\n"
            "bytes differ from what the build recorded, is refused by name.\n"
            "\n"
            "Its template_cuts declare the Locator templates the build cuts\n"
            "into generated/templates/, naming each source image by sha256 and\n"
            "never by path, so no project references a screenshot. --frames-root\n"
            "names the directory those sources are read from, where the bytes of\n"
            "hash H are the file H.png; it is outside the project because a\n"
            "corpus of captures is a property of the machine. A declared cut\n"
            "whose source is not there is refused by name -- build, check and\n"
            "freeze alike -- and never quietly skipped.\n";
    }
}
