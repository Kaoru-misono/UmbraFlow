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
#include <set>
#include <span>
#include <string>
#include <string_view>
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
        };

        constexpr auto k_projectFlags = std::array{
            ProjectFlagDefinition{"--source", ProjectFlag::Source},
            ProjectFlagDefinition{"--build", ProjectFlag::Build},
            ProjectFlagDefinition{"--input", ProjectFlag::Input},
            ProjectFlagDefinition{"--release", ProjectFlag::Release},
            ProjectFlagDefinition{"--frames-root", ProjectFlag::FramesRoot},
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
                }
            }
            return parsed;
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
        // schema/umbraflow-project-v1.schema.json and of the type stated
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

        // What the deployment declaration contributes to the spec the kit is
        // given: every deployment names its declared tool catalog source and
        // its artifact blobs, and the artifact-roots registration is exactly
        // the closure those blobs state. The registration is derived, never
        // declared: a project that stated its own artifact roots could state
        // roots that disagree with its own closure.
        [[nodiscard]]
        auto attachDeploymentDeclarations(ProjectBuildSpec& spec) -> Status
        {
            UF_TRY_VALUE(document, readProjectRootDocument(spec.sourceDirectory));
            auto toolCatalogs  = std::vector<ToolCatalogDeclaration>{};
            auto artifactBlobs = std::vector<ProjectArtifactBlobSpec>{};
            auto blobNames     = std::set<std::string>{};
            for (auto const& deployment : member(document, "deployments").items())
            {
                UF_TRY_VALUE(
                    catalog,
                    declaredToolCatalog(spec.sourceDirectory, deployment)
                );
                toolCatalogs.emplace_back(std::move(catalog));
                for (auto const& blob : member(deployment, "artifact_blobs").items())
                {
                    auto const name = std::string{member(blob, "name").string()};
                    if (!blobNames.emplace(name).second)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            std::format(
                                "the deployment declarations name the artifact "
                                "root {} more than once",
                                name
                            )
                        );
                    }
                    artifactBlobs.emplace_back(ProjectArtifactBlobSpec{
                        .name        = std::move(name),
                        .sourceInput = std::filesystem::path{
                            member(blob, "path").string()
                        },
                    });
                }
            }
            spec.toolCatalogs  = std::move(toolCatalogs);
            spec.artifactBlobs = std::move(artifactBlobs);
            spec.registration.artifactBlobNames.assign(
                blobNames.begin(),
                blobNames.end()
            );
            return ok();
        }

        [[nodiscard]]
        auto parseProjectInit(
            std::span<std::string const> raw
        ) -> Result<ProjectInitSpec>
        {
            UF_TRY_VALUE(parsed, parseProjectFlags(raw));
            if (!parsed.source || !parsed.build)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project init requires --source PATH and --build PATH"
                );
            }
            if (parsed.inputs.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project init requires at least one --input RELATIVE_PATH"
                );
            }
            if (parsed.release || parsed.framesRoot)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project init accepts only --source, --build and --input"
                );
            }
            return ProjectInitSpec{
                .sourceDirectory = std::move(*parsed.source),
                .buildDirectory  = std::move(*parsed.build),
                .inputs          = std::move(parsed.inputs),
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
            if (!parsed.source || !parsed.build)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project {} requires --source PATH and --build PATH",
                        action
                    )
                );
            }
            if (!parsed.inputs.empty() || parsed.release)
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
            return ParsedProjectBuild{
                .spec = ProjectBuildSpec{
                    .sourceDirectory = std::move(*parsed.source),
                    .buildDirectory  = std::move(*parsed.build),
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
            if (!parsed.source || !parsed.build || !parsed.release)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project freeze requires --source PATH, --build PATH and "
                    "--release PATH"
                );
            }
            if (!parsed.inputs.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project freeze does not accept --input"
                );
            }
            return ParsedProjectFreeze{
                .spec = ProjectFreezeSpec{
                    .candidate = ProjectBuildSpec{
                        .sourceDirectory = std::move(*parsed.source),
                        .buildDirectory  = std::move(*parsed.build),
                        .toolCatalogs    = {},
                    },
                    .releaseRoot = std::move(*parsed.release),
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
            auto const spec = parseProjectInit(raw);
            if (!spec)
            {
                std::cerr << spec.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const initialized = initProject(*spec);
            if (!initialized)
            {
                return reportProjectError(initialized.error());
            }
            std::cout << std::format(
                "project init: source=\"{}\" build=\"{}\" inputs={}\n",
                spec->sourceDirectory.string(),
                spec->buildDirectory.string(),
                spec->inputs.size()
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
            "  project init --source PATH --build PATH --input RELATIVE_PATH "
            "[--input RELATIVE_PATH ...]\n"
            "  project build --source PATH --build PATH "
            "[--frames-root PATH]\n"
            "  project check --source PATH --build PATH "
            "[--frames-root PATH]\n"
            "  project freeze --source PATH --build PATH --release PATH "
            "[--frames-root PATH]\n"
            "  project run --release RELEASE_DIRECTORY\n"
            "\n"
            "Initializes the declared source inputs, builds only into the build\n"
            "directory, checks the candidate, freezes a content-addressed\n"
            "read-only release, and runs only a verified immutable release.\n"
            "A declared declarative-tools/PLUGIN_ID/NAME.json input generates\n"
            "generated/adapters/PLUGIN_ID/NAME.luau. The source directory must\n"
            "hold umbraflow-project.json at its root, declared or not; build\n"
            "and check judge it against the published project schema, which\n"
            "requires a plugin_justification of every deployment whose\n"
            "plugin_authoring is hand-written and refuses one from every\n"
            "deployment whose plugin_authoring is generated.\n"
            "\n"
            "Every deployment also names its declared tool catalog source and\n"
            "its artifact blobs. build and check read the first as the Tool\n"
            "Catalog document it declares -- a source the tree does not hold is\n"
            "refused by name, like a declared cut's missing corpus -- and read\n"
            "the second back into generated/artifact-blobs/, with\n"
            "generated/registrations/artifact-roots-v1.json stating exactly the\n"
            "closure those blobs declare. Nothing states a registration\n"
            "separately: a project that named its own artifact roots could name\n"
            "roots that disagree with its own closure.\n"
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
