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
            std::vector<std::filesystem::path>   inputs{};
        };

        constexpr auto k_projectFlags = std::array{
            ProjectFlagDefinition{"--source", ProjectFlag::Source},
            ProjectFlagDefinition{"--build", ProjectFlag::Build},
            ProjectFlagDefinition{"--input", ProjectFlag::Input},
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
            return ProjectInitSpec{
                .sourceDirectory = std::move(*parsed.source),
                .buildDirectory  = std::move(*parsed.build),
                .inputs          = std::move(parsed.inputs),
            };
        }

        [[nodiscard]]
        auto parseProjectDirectories(
            std::span<std::string const> raw,
            std::string_view action
        ) -> Result<ProjectDirectories>
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
            if (!parsed.inputs.empty())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "project {} does not accept --input",
                        action
                    )
                );
            }
            return ProjectDirectories{
                .sourceDirectory = std::move(*parsed.source),
                .buildDirectory  = std::move(*parsed.build),
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
            auto const directories = parseProjectDirectories(raw, "build");
            if (!directories)
            {
                std::cerr << directories.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const built = buildProject(*directories);
            if (!built)
            {
                return reportProjectError(built.error());
            }
            std::cout << std::format(
                "project build: build=\"{}\"\n",
                directories->buildDirectory.string()
            );
            return ProjectExitCode::Success;
        }

        [[nodiscard]]
        auto runProjectCheck(
            std::span<std::string const> raw
        ) -> ProjectExitCode
        {
            auto const directories = parseProjectDirectories(raw, "check");
            if (!directories)
            {
                std::cerr << directories.error().message() << '\n';
                std::cerr << projectUsageText();
                return ProjectExitCode::Failure;
            }

            auto const checked = checkProject(*directories);
            if (!checked)
            {
                return reportProjectError(checked.error());
            }
            std::cout << std::format(
                "project check: source=\"{}\" build=\"{}\"\n",
                directories->sourceDirectory.string(),
                directories->buildDirectory.string()
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
            "\n"
            "Initializes the declared source inputs, builds only into the build\n"
            "directory, and checks the declared inputs and build receipt.\n";
    }
}
