#include "args.hpp"
#include "capture-mode.hpp"
#include "guard.hpp"
#include "log-jsonl.hpp"
#include "pipeline.hpp"
#include "shutdown.hpp"

#include <error-text.hpp>
#include <target-setup.hpp>

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/enum-reflection.hpp>
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
    // Borrowed from the input agent, which owns the shared entry substrate
    // this frozen demo was split away from.
    using input_agent::buildSelector;
    using input_agent::createCaptureSession;
    using input_agent::formatAutomationError;

    namespace
    {
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
                        .withOutcome("ok")
                        .withDetail(
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
                    .withOutcome("unknown")
                    .withDetail(
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
                        .withOutcome("ok")
                        .withDetail(std::string{enumName(dpiDeclaration).value_or("Unknown")})
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
            auto const selector = buildSelector(args.selector);
            UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));
            auto const windowHandle = resolved.windowHandle();
            auto const generation = resolved.currentGeneration();
            auto const client = resolved.clientSize();
            auto const process = resolved.identity().process();
            auto const sessionId = CaptureSessionId{1};
            UF_TRY_VALUE(
                options,
                WgcCaptureOptions::create(args.stallTimeout, false)
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
                        .withOutcome("ok")
                        .withDetail(
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
                    args.homeTemplate,
                    "home",
                    args.homeRoi
                )
            );
            UF_TRY_VALUE(
                resultTemplate,
                loadTemplate(
                    args.resultTemplate,
                    "result",
                    args.resultRoi
                )
            );
            UF_TRY_VALUE(
                resetTemplate,
                loadTemplate(
                    args.resetTemplate,
                    "reset",
                    args.resetRoi
                )
            );
            auto const templates = Templates{
                .home   = std::move(homeTemplate),
                .result = std::move(resultTemplate),
                .reset  = std::move(resetTemplate),
            };
            auto const config = LoopConfig{
                .loops             = args.loops,
                .threshold         = args.threshold,
                .maxActionFrameAge = args.maxActionFrameAge,
                .transitionTimeout = k_defaultTransitionTimeout,
                .guardPolicy       = GuardPolicy::forMode(args.mode),
                .clickDelay        = args.clickDelay,
                .seed              = args.seed,
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
                        .withOutcome("ok")
                        .withDetail(
                            std::format(
                                "os_build={} cursor_capture_disabled={} border_required={}",
                                hygiene.osBuild,
                                hygiene.cursorCaptureDisabled,
                                hygiene.borderRequired
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
            UF_TRY_VALUE(log, JsonlLog::create(args.log));

            auto outcome       = runWithLog(args, log);
            auto terminalWrite = ok();
            if (!outcome)
            {
                terminalWrite = log.write(
                    LogLine{"run", "fatal"}
                        .withOutcome("error")
                        .withDetail(formatAutomationError(outcome.error()))
                );
            }
            auto flush = log.flush();
            if (!outcome)
            {
                return std::unexpected{std::move(outcome).error()};
            }

            UF_TRY(std::move(terminalWrite));
            UF_TRY(std::move(flush));
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
            // The annotation front-end left this binary on 2026-07-31. The
            // spelling survives only to say where it went, because recorded
            // procedures and session scripts still reach for it here; without
            // this branch the demo parser would answer "unknown argument" and
            // a reader would take that for a broken build.
            std::cerr
                << "m0-demo no longer serves the input agent; it is its own "
                   "program now.\n"
                   "Run umbra-input-agent with the same arguments:\n"
                   "  umbra-input-agent --hwnd N|0xHEX --queue PATH "
                   "--results PATH --output-dir DIR\n";
            return EXIT_FAILURE;
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
            outcome->attempted,
            outcome->succeeded,
            outcome->guardViolations,
            outcome->stopped,
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
