#include "panels.hpp"
#include "../workbench-app.hpp"

#include "platform/windows-capture-source.hpp"
#include "platform/windows-file-dialog.hpp"
#include "platform/windows-gui-shell.hpp"
#include "project-persistence.hpp"
#include "source-ingestion.hpp"

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_windowTitle = std::string_view{"Umbra Workbench"};

        struct Options final
        {
            std::optional<uint32>                smokeFrames{};
            std::optional<std::filesystem::path> projectRoot{};
        };

        [[nodiscard]]
        auto parseSmokeFrames(std::string_view value) -> std::optional<uint32>
        {
            auto parsed     = uint32{0};
            auto const last = value.data() + value.size();
            auto const [pointer, code] = std::from_chars(
                value.data(),
                last,
                parsed
            );
            if (code != std::errc{} || pointer != last)
            {
                return std::nullopt;
            }
            return parsed;
        }

        [[nodiscard]]
        auto parseOptions(std::span<std::string const> arguments) -> Result<Options>
        {
            auto options = Options{};
            for (auto index = std::size_t{0}; index < arguments.size(); ++index)
            {
                auto const& argument = arguments[index];
                if (argument == "--smoke")
                {
                    if (index + 1U >= arguments.size())
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "--smoke requires a frame count"
                        );
                    }
                    ++index;
                    auto const frames = parseSmokeFrames(arguments[index]);
                    if (!frames || *frames == 0U)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "--smoke requires a positive frame count"
                        );
                    }
                    options.smokeFrames = frames;
                }
                else if (argument.starts_with("--"))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format("unknown option \"{}\"", argument)
                    );
                }
                else if (!options.projectRoot.has_value())
                {
                    options.projectRoot = std::filesystem::path{argument};
                }
                else
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "at most one project root may be given"
                    );
                }
            }
            return options;
        }

        [[nodiscard]]
        auto makeAppState(Options const& options) -> Result<AppState>
        {
            if (options.projectRoot.has_value())
            {
                UF_TRY_VALUE(
                    loaded,
                    loadAuthoringProject(*options.projectRoot)
                );
                return AppState{
                    *options.projectRoot,
                    std::move(loaded.document),
                    std::move(loaded.sources),
                };
            }
            return AppState::createEmpty(std::filesystem::current_path());
        }

        [[nodiscard]]
        auto runWorkbench(Options const& options) -> Status
        {
            UF_TRY_VALUE(state, makeAppState(options));

            auto const config = platform::GuiShellConfig{
                .title       = std::string{k_windowTitle},
                .width       = 1280U,
                .height      = 720U,
                .smokeFrames = options.smokeFrames,
            };
            UF_TRY_VALUE(shell, platform::GuiShell::create(config));

            auto const logPath = state.projectRoot() / "workbench.log";

            auto services = WorkbenchServices{
                .textureFor =
                    [&shell](
                        annotation::AuthoringSourceAsset const& asset
                    ) -> Result<platform::GpuSourceTexture>
                    {
                        return shell.textures().textureFor(asset);
                    },
                .pruneTextures =
                    [&shell](
                        std::span<annotation::AuthoringSource const> sources
                    )
                    {
                        shell.textures().pruneTo(sources);
                    },
                .pickPngToImport =
                    []() -> Result<std::optional<std::filesystem::path>>
                    {
                        return platform::openPngFileDialog();
                    },
                .captureFromTarget =
                    [](
                        annotation::SourceId id,
                        std::string const& titleSubstring
                    ) -> Result<IngestedSource>
                    {
                        return platform::captureSourceFromTargetTitle(
                            id,
                            titleSubstring
                        );
                    },
                .appendLog =
                    [logPath](
                        LogSeverity severity,
                        std::string_view timestamp,
                        std::string_view message
                    ) -> void
                    {
                        auto stream = std::ofstream{
                            logPath,
                            std::ios::app | std::ios::binary
                        };
                        if (!stream.is_open())
                        {
                            return;
                        }
                        stream << formatLogLine(severity, timestamp, message)
                               << '\n';
                    },
            };
            auto ui = PanelUiState{};

            return shell.run(
                [&state, &services, &ui]() -> void
                {
                    drawWorkbench(state, services, ui);
                }
            );
        }

        [[nodiscard]]
        auto dispatch(std::span<std::string const> arguments) -> int
        {
            auto const options = parseOptions(arguments);
            if (!options)
            {
                std::cerr << "umbra-workbench: " << toString(options.error()) << '\n';
                return EXIT_FAILURE;
            }

            auto const result = runWorkbench(*options);
            if (!result)
            {
                std::cerr << "umbra-workbench: " << toString(result.error()) << '\n';
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
    }
}

auto main(int argumentCount, char const* const* p_arguments) -> int
{
    try
    {
        auto const convertedArgumentCount = uf::checkedCast<std::size_t>(
            argumentCount
        );
        if (!convertedArgumentCount || *convertedArgumentCount == 0U)
        {
            std::cerr << "umbra-workbench: invalid process argument vector\n";
            return EXIT_FAILURE;
        }
        auto const arguments = std::span<char const* const>{
            p_arguments,
            *convertedArgumentCount
        };
        auto raw = std::vector<std::string>{};
        for (auto const* argument : arguments.subspan(1U))
        {
            raw.emplace_back(argument);
        }

        return uf::workbench::dispatch(raw);
    }
    catch (std::exception const& error)
    {
        std::cerr << "umbra-workbench exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    catch (...)
    {
        std::cerr << "umbra-workbench exception: unknown failure\n";
        return EXIT_FAILURE;
    }
}
