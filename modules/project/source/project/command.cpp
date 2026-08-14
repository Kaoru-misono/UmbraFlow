#include "command.hpp"

#include "project-kit.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
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
            std::vector<std::filesystem::path>   inputs{};
        };

        constexpr auto k_projectFlags = std::array{
            ProjectFlagDefinition{"--source", ProjectFlag::Source},
            ProjectFlagDefinition{"--build", ProjectFlag::Build},
            ProjectFlagDefinition{"--input", ProjectFlag::Input},
            ProjectFlagDefinition{"--release", ProjectFlag::Release},
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
                }
            }
            return parsed;
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
            if (parsed.release)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "project init does not accept --release"
                );
            }
            return ProjectInitSpec{
                .sourceDirectory = std::move(*parsed.source),
                .buildDirectory  = std::move(*parsed.build),
                .inputs          = std::move(parsed.inputs),
            };
        }

        [[nodiscard]]
        auto parseProjectBuildSpec(
            std::span<std::string const> raw,
            std::string_view action
        ) -> Result<ProjectBuildSpec>
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
                        "project {} accepts only --source and --build",
                        action
                    )
                );
            }
            return ProjectBuildSpec{
                .sourceDirectory = std::move(*parsed.source),
                .buildDirectory  = std::move(*parsed.build),
                .toolCatalogs    = {},
            };
        }

        [[nodiscard]]
        auto parseProjectFreeze(
            std::span<std::string const> raw
        ) -> Result<ProjectFreezeSpec>
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
            return ProjectFreezeSpec{
                .candidate = ProjectBuildSpec{
                    .sourceDirectory = std::move(*parsed.source),
                    .buildDirectory  = std::move(*parsed.build),
                    .toolCatalogs    = {},
                },
                .releaseRoot = std::move(*parsed.release),
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
            auto const spec = parseProjectBuildSpec(raw, "build");
            if (!spec)
            {
                std::cerr << spec.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const built = buildProject(*spec, {});
            if (!built)
            {
                return reportProjectError(built.error());
            }
            std::cout << std::format(
                "project build: build=\"{}\"\n",
                spec->buildDirectory.string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectCheck(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto const spec = parseProjectBuildSpec(raw, "check");
            if (!spec)
            {
                std::cerr << spec.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const checked = checkProject(*spec, {});
            if (!checked)
            {
                return reportProjectError(checked.error());
            }
            std::cout << std::format(
                "project check: source=\"{}\" build=\"{}\"\n",
                spec->sourceDirectory.string(),
                spec->buildDirectory.string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectFreeze(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto const spec = parseProjectFreeze(raw);
            if (!spec)
            {
                std::cerr << spec.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const release = freezeProject(*spec, {});
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
            "  project build --source PATH --build PATH\n"
            "  project check --source PATH --build PATH\n"
            "  project freeze --source PATH --build PATH --release PATH\n"
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
            "deployment whose plugin_authoring is generated.\n";
    }
}
