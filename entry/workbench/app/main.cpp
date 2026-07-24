#include "panels.hpp"
#include "workbench-app.hpp"

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
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
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
            std::optional<uint32>                m_smokeFrames{};
            std::optional<std::filesystem::path> m_projectRoot{};
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
                    options.m_smokeFrames = frames;
                }
                else if (argument.starts_with("--"))
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        std::format("unknown option \"{}\"", argument)
                    );
                }
                else if (!options.m_projectRoot.has_value())
                {
                    options.m_projectRoot = std::filesystem::path{argument};
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
            if (options.m_projectRoot.has_value())
            {
                UF_TRY_VALUE(
                    loaded,
                    loadAuthoringProject(*options.m_projectRoot)
                );
                return AppState{
                    *options.m_projectRoot,
                    std::move(loaded.m_document),
                    std::move(loaded.m_sources),
                };
            }
            return AppState::createEmpty(std::filesystem::current_path());
        }

        [[nodiscard]]
        auto runWorkbench(Options const& options) -> Status
        {
            UF_TRY_VALUE(state, makeAppState(options));

            auto const config = platform::GuiShellConfig{
                .m_title       = std::string{k_windowTitle},
                .m_width       = 1280U,
                .m_height      = 720U,
                .m_smokeFrames = options.m_smokeFrames,
            };
            UF_TRY_VALUE(shell, platform::GuiShell::create(config));

            auto services = WorkbenchServices{
                .m_textureFor =
                    [&shell](
                        annotation::AuthoringSourceAsset const& asset
                    ) -> Result<platform::GpuSourceTexture>
                    {
                        return shell.textures().textureFor(asset);
                    },
                .m_pickPngToImport =
                    []() -> Result<std::optional<std::filesystem::path>>
                    {
                        return platform::openPngFileDialog();
                    },
                .m_captureFromTarget =
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
