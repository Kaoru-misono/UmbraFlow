#include "args.hpp"
#include "capture-mode.hpp"
#include "guard.hpp"
#include "input-agent.hpp"
#include "log-jsonl.hpp"
#include "pipeline.hpp"
#include "shutdown.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::m0_demo
{
    namespace
    {
        [[nodiscard]]
        auto dpiDeclarationName(DpiDeclaration declaration) noexcept -> std::string_view
        {
            switch (declaration)
            {
            case DpiDeclaration::Declared: return "Declared";
            case DpiDeclaration::AlreadyDeclared: return "AlreadyDeclared";
            }
            return "Unknown";
        }

        [[nodiscard]]
        auto logIntegrity(
            JsonlLog& log,
            std::string_view role,
            std::optional<IntegrityLevel> integrity
        ) -> Status
        {
            if (integrity)
            {
                return log.write(
                    LogLine{"setup", "integrity"}
                        .outcome("ok")
                        .detail(
                            std::format(
                                "{} rid={:#x} level={}",
                                role,
                                integrity->rid(),
                                integrity->label()
                            )
                        )
                );
            }
            return log.write(
                LogLine{"setup", "integrity"}
                    .outcome("unknown")
                    .detail(
                        std::format(
                            "{} integrity unreadable (access denied?)",
                            role
                        )
                    )
            );
        }

        [[nodiscard]]
        auto runWithLog(
            Args const& args,
            JsonlLog& log
        ) -> Result<RunSummary>
        {
            UF_TRY_VALUE(dpiDeclaration, ensurePerMonitorAwareV2());
            UF_TRY(
                log.write(
                    LogLine{"setup", "dpi_declared"}
                        .outcome("ok")
                        .detail(std::string{dpiDeclarationName(dpiDeclaration)})
                )
            );

            UF_TRY_VALUE(
                consoleControl,
                installConsoleControlHandler()
            );
            UF_TRY(
                logIntegrity(
                    log,
                    "automation",
                    currentProcessIntegrity()
                )
            );

            UF_TRY_VALUE(candidates, enumerateCandidates());
            auto const selector = buildSelector(args.m_selector);
            UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));
            auto const windowHandle = resolved.windowHandle();
            auto const generation = resolved.currentGeneration();
            auto const client = resolved.clientSize();
            auto const process = resolved.identity().process();
            auto const sessionId = SessionId{1};
            UF_TRY_VALUE(
                options,
                WgcCaptureOptions::create(args.m_stallTimeout, false)
            );
            UF_TRY_VALUE(
                session,
                createCaptureSession(
                    resolved,
                    sessionId,
                    options
                )
            );
            UF_TRY(
                log.write(
                    LogLine{"discovery", "resolved"}
                        .outcome("ok")
                        .detail(
                            std::format(
                                "pid={} client={}x{}",
                                process.value(),
                                client.width(),
                                client.height()
                            )
                        )
                )
            );

            UF_TRY(
                logIntegrity(
                    log,
                    "target",
                    processIntegrity(process)
                )
            );

            UF_TRY_VALUE(
                homeTemplate,
                loadTemplate(
                    args.m_homeTemplate,
                    "home",
                    args.m_homeRoi
                )
            );
            UF_TRY_VALUE(
                resultTemplate,
                loadTemplate(
                    args.m_resultTemplate,
                    "result",
                    args.m_resultRoi
                )
            );
            UF_TRY_VALUE(
                resetTemplate,
                loadTemplate(
                    args.m_resetTemplate,
                    "reset",
                    args.m_resetRoi
                )
            );
            auto const templates = Templates{
                .m_home   = std::move(homeTemplate),
                .m_result = std::move(resultTemplate),
                .m_reset  = std::move(resetTemplate),
            };
            auto const config = LoopConfig{
                .m_loops             = args.m_loops,
                .m_threshold         = args.m_threshold,
                .m_maxActionFrameAge = args.m_maxActionFrameAge,
                .m_transitionTimeout = g_defaultTransitionTimeout,
                .m_guardPolicy       = GuardPolicy::forMode(args.m_mode),
                .m_clickDelay        = args.m_clickDelay,
                .m_seed              = args.m_seed,
            };

            UF_TRY_VALUE(
                delivery,
                DeliveryTarget::create(
                    windowHandle,
                    sessionId,
                    generation,
                    client.width(),
                    client.height()
                )
            );
            auto const hygiene = session.hygiene();
            UF_TRY(
                log.write(
                    LogLine{"setup", "session_created"}
                        .outcome("ok")
                        .detail(
                            std::format(
                                "os_build={} cursor_capture_disabled={} border_required={}",
                                hygiene.m_osBuild,
                                hygiene.m_cursorCaptureDisabled,
                                hygiene.m_borderRequired
                            )
                        )
                )
            );

            auto outcome = runPipeline(
                resolved,
                std::move(session),
                delivery,
                templates,
                config,
                log
            );
            auto consoleClose = consoleControl.close();
            if (!outcome)
            {
                return std::unexpected{std::move(outcome).error()};
            }
            UF_TRY(std::move(consoleClose));
            return outcome;
        }

        [[nodiscard]]
        auto run(
            std::span<std::string const> raw
        ) -> Result<RunSummary>
        {
            UF_TRY_VALUE(args, parseArguments(raw));
            UF_TRY_VALUE(log, JsonlLog::create(args.m_log));

            auto outcome       = runWithLog(args, log);
            auto terminalWrite = ok();
            if (!outcome)
            {
                terminalWrite = log.write(
                    LogLine{"run", "fatal"}
                        .outcome("error")
                        .detail(formatAutomationError(outcome.error()))
                );
            }
            auto flush = log.flush();
            if (!outcome)
            {
                return std::unexpected{std::move(outcome).error()};
            }

            UF_TRY(terminalWrite);
            UF_TRY(flush);
            return *std::move(outcome);
        }

        auto writeUnhandledException(std::exception const& error) noexcept -> void
        {
            static_cast<void>(std::fputs("m0-demo exception: ", stderr));
            static_cast<void>(std::fputs(error.what(), stderr));
            static_cast<void>(std::fputc('\n', stderr));
        }

        auto writeUnknownException() noexcept -> void
        {
            static_cast<void>(
                std::fputs("m0-demo exception: unknown failure\n", stderr)
            );
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
            std::cerr << "m0-demo error: invalid process argument vector\n";
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
        if (!raw.empty() && raw.front() == "capture")
        {
            auto const captureRaw = std::span<std::string const>{raw}.subspan(1);
            auto const outcome = uf::m0_demo::runCapture(captureRaw);
            if (!outcome)
            {
                std::cerr
                    << "m0-demo capture error: "
                    << uf::m0_demo::formatAutomationError(outcome.error())
                    << '\n';
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }
        if (!raw.empty() && raw.front() == "input-agent")
        {
            auto const agentRaw = std::span<std::string const>{raw}.subspan(1);
            auto const outcome = uf::m0_demo::runInputAgent(agentRaw);
            if (!outcome)
            {
                std::cerr
                    << "m0-demo input-agent error: "
                    << uf::m0_demo::formatAutomationError(outcome.error())
                    << '\n';
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        }

        auto const outcome = uf::m0_demo::run(raw);
        if (!outcome)
        {
            std::cerr
                << "m0-demo error: "
                << uf::m0_demo::formatAutomationError(outcome.error())
                << '\n';
            return EXIT_FAILURE;
        }

        std::cerr << std::format(
            "m0-demo: attempted={} succeeded={} guard_violations={} stopped={} passed={}\n",
            outcome->m_attempted,
            outcome->m_succeeded,
            outcome->m_guardViolations,
            outcome->m_stopped,
            outcome->passed()
        );
        return outcome->passed() ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    catch (std::exception const& error)
    {
        uf::m0_demo::writeUnhandledException(error);
        return EXIT_FAILURE;
    }
    catch (...)
    {
        uf::m0_demo::writeUnknownException();
        return EXIT_FAILURE;
    }
}
