#include "command.hpp"

#include "platform/process-run.hpp"
#include "project-kit.hpp"
#include "release-bundle.hpp"

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

        // --release is carried as text rather than as a path because the two
        // verbs that take it mean two different things by it: freeze and run
        // name a directory of this project's own frozen output, and upgrade
        // names a framework release on the host. Each verb converts what it
        // was given; nothing here decides which of the two it is.
        struct ParsedProjectFlags final
        {
            std::optional<std::filesystem::path> source{};
            std::optional<std::filesystem::path> build{};
            std::optional<std::string>           release{};
            std::optional<std::filesystem::path> framesRoot{};
            std::optional<std::string>           plugin{};
            std::optional<std::string>           pluginId{};
        };

        constexpr auto k_projectFlags = std::array{
            ProjectFlagDefinition{"--source", ProjectFlag::Source},
            ProjectFlagDefinition{"--build", ProjectFlag::Build},
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
                case ProjectFlag::Release:
                    if (parsed.release)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "project argument \"--release\" appears more than once"
                        );
                    }
                    parsed.release = value;
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

        // The two directories every verb works between: the source tree the
        // declaration and its files live in, and the build tree the generated
        // artifacts and the build receipt live in.
        struct ProjectDirectories final
        {
            std::filesystem::path sourceDirectory{};
            std::filesystem::path buildDirectory{};
        };

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

        struct ParsedProjectInit final
        {
            ProjectBuildSpec                   spec{};
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
                .spec = ProjectBuildSpec{
                    .sourceDirectory = std::move(directories.sourceDirectory),
                    .buildDirectory  = std::move(directories.buildDirectory),
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
            if (parsed.release || parsed.plugin || parsed.pluginId)
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
            if (parsed.plugin || parsed.pluginId)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project freeze does not accept --plugin or --plugin-id"
                );
            }
            UF_TRY_VALUE(directories, projectDirectories(parsed));
            auto release = parsed.release
                ? std::filesystem::path{*parsed.release}
                : directories.sourceDirectory / "work" / "release";
            return ParsedProjectFreeze{
                .spec = ProjectFreezeSpec{
                    .candidate = ProjectBuildSpec{
                        .sourceDirectory = std::move(directories.sourceDirectory),
                        .buildDirectory  = std::move(directories.buildDirectory),
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
                || parsed.plugin
                || parsed.pluginId
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project run requires only --release PATH"
                );
            }
            return std::filesystem::path{*parsed.release};
        }

        [[nodiscard]]
        auto parseProjectUpgrade(
            std::span<std::string const> raw
        ) -> Result<ProjectUpgradeSpec>
        {
            UF_TRY_VALUE(parsed, parseProjectFlags(raw));
            if (parsed.build || parsed.framesRoot || parsed.plugin || parsed.pluginId)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project upgrade accepts only --source and --release"
                );
            }
            UF_TRY_VALUE(directories, projectDirectories(parsed));
            return ProjectUpgradeSpec{
                .sourceDirectory = std::move(directories.sourceDirectory),
                .releaseOverride = parsed.release
                    ? std::move(*parsed.release)
                    : std::string{},
            };
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

        // Pull the new binaries, then read what is left to do.
        //
        // The two halves are one command because separating them costs the
        // author a round trip they cannot skip: the schema a declaration is
        // judged against is compiled into the binary, so nothing can say what a
        // release expects until that release is installed. Install first,
        // report second -- and never the other way round, because a refusal to
        // install would leave the author editing against a shape no binary
        // present can confirm.
        //
        // The report is produced by RUNNING the installed binary rather than by
        // this process, for the same reason. `check` is what it runs, because
        // `check` is what the author runs next anyway.
        [[nodiscard]]
        auto runProjectUpgrade(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto const parsed = parseProjectUpgrade(raw);
            if (!parsed)
            {
                std::cerr << parsed.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const installed = upgradeReleaseBundle(*parsed);
            if (!installed)
            {
                return reportProjectError(installed.error());
            }
            std::cout << std::format(
                "project upgrade: release={} bundle=\"{}\"\n",
                installed->release,
                installed->bundleDirectory.string()
            );
            std::cout.flush();

            auto const arguments = std::vector<std::string>{
                installed->projectExecutable.string(),
                "check",
                "--source",
                parsed->sourceDirectory.string(),
            };
            auto const status = runProcess(arguments);
            if (!status)
            {
                return reportProjectError(status.error());
            }
            return *status == 0
                ? ProjectExitCode::Success
                : ProjectExitCode::Failure;
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
            ProjectCommand{"upgrade", &runProjectUpgrade},
            ProjectCommand{"init", &runProjectInit},
            ProjectCommand{"build", &runProjectBuild},
            ProjectCommand{"check", &runProjectCheck},
            ProjectCommand{"freeze", &runProjectFreeze},
            ProjectCommand{"run", &runProjectRun},
        };

        // Which source tree this invocation is about, established before any
        // verb runs and without judging the command line.
        //
        // It exists for one job: every invocation reconciles the bundle
        // directories first, and that has to happen even when the rest of the
        // command line turns out to be wrong. A verb's own parser is the
        // authority on what it accepts and refuses; this only looks for the
        // one flag that says where.
        [[nodiscard]]
        auto sourceDirectoryOf(
            std::span<std::string const> raw
        ) -> std::filesystem::path
        {
            for (auto index = std::size_t{0}; index + 1U < raw.size(); ++index)
            {
                if (raw[index] == "--source")
                {
                    return std::filesystem::path{raw[index + 1U]};
                }
            }
            auto error         = std::error_code{};
            auto const current = std::filesystem::current_path(error);
            return error ? std::filesystem::path{} : current;
        }
    }

    auto runProjectCommand(
        std::span<std::string const> raw
    ) -> ProjectExitCode
    {
        // The first act of every invocation, before the command line is even
        // judged. A completed upgrade leaves the previous bundle beside the new
        // one because a directory holding a running executable cannot be
        // deleted on this platform; by the time any later command runs, nothing
        // holds it and the delete succeeds. Doing it here rather than in
        // upgrade is what makes that true of the NEXT command whichever one it
        // is.
        auto const source = sourceDirectoryOf(raw);
        if (!source.empty())
        {
            auto const reconciled = reconcileBundleDirectories(source);
            if (!reconciled)
            {
                return reportProjectError(reconciled.error());
            }
            std::cerr << *reconciled;
        }

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
            "  project upgrade [--source PATH] [--release NAME]\n"
            "  project init [--source PATH] [--build PATH] "
            "[--plugin generated|hand-written --plugin-id NAME]\n"
            "  project build [--source PATH] [--build PATH] "
            "[--frames-root PATH]\n"
            "  project check [--source PATH] [--build PATH] "
            "[--frames-root PATH]\n"
            "  project freeze [--source PATH] [--build PATH] [--release PATH] "
            "[--frames-root PATH]\n"
            "  project run --release RELEASE_DIRECTORY\n"
            "\n"
            "upgrade installs the framework release umbraflow-kit.json selects\n"
            "-- or the one --release NAME names for this run -- under\n"
            "<source>/umbraflow-bin, verifying every artifact against the\n"
            "sha256 its manifest declares before anything is swapped into\n"
            "place. It installs a release whether or not this project's\n"
            "declaration already matches it, and then runs the newly installed\n"
            "binary's own check so one command yields new binaries plus the\n"
            "whole remaining work list. The previous bundle is left beside the\n"
            "new one as umbraflow-bin.PREVIOUS_RELEASE, because a directory\n"
            "holding a running executable cannot be deleted; the next project\n"
            "command removes it. That leftover is not a rollback -- to go back,\n"
            "pin release in umbraflow-kit.json to the older name and upgrade\n"
            "again.\n"
            "\n"
            "init creates a starter Project when umbraflow-project.json is\n"
            "absent and gives the build tree somewhere to land. --plugin and\n"
            "--plugin-id are required together only for that first init.\n"
            "Source defaults to the current directory, build to\n"
            "<source>/work/build and release to <source>/work/release. build\n"
            "materializes generated artifacts, check judges them, freeze\n"
            "publishes a content-addressed read-only release, and run accepts\n"
            "only a verified immutable release.\n"
            "\n"
            "check is the verb to re-run after every edit: it reaches no\n"
            "network, writes nothing into umbraflow-bin, and derives the file\n"
            "set it judges from umbraflow-project.json itself, so no earlier\n"
            "command has to be repeated first.\n"
            "\n"
            "A derived declarative-tools/PLUGIN_ID/NAME.json input generates\n"
            "generated/adapters/PLUGIN_ID/NAME.luau. The source directory must\n"
            "hold umbraflow-project.json after init; build\n"
            "and check judge it against the published project schema, which\n"
            "requires a plugin_justification of every deployment whose\n"
            "plugin_authoring is hand-written and refuses one from every\n"
            "deployment whose plugin_authoring is generated. They also apply\n"
            "the two joins no schema can state -- every Tool name inside the\n"
            "namespace its deployment's plugin_id owns, and every tool binding\n"
            "paired with a declared Tool and an exported closure entry -- so a\n"
            "declaration check accepts is one umbra-flow open accepts.\n"
            "\n"
            "Every deployment also names its declared tool catalog, closed\n"
            "module set and typed resources. build and check materialize the\n"
            "exact execution bytes under generated/modules/ and\n"
            "generated/resources/. generated/registrations/DEPLOYMENT.json\n"
            "records the module-manifest and running environment identities\n"
            "plus every resource kind, digest and size; project authors type no\n"
            "digest in umbraflow-project.json.\n"
            "\n"
            "The file set a build acts on is derived from\n"
            "umbraflow-project.json itself -- the document, every module of\n"
            "every closure or the declaration a generated module is rendered\n"
            "from, and every resource -- and never from a list kept beside it.\n"
            "build records each of those by digest, and check holds each\n"
            "against that record: a declared file the tree does not hold, or\n"
            "whose bytes differ from what the build recorded, is refused by\n"
            "name.\n"
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
