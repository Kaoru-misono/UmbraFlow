#include "args.hpp"
#include "guard.hpp"
#include "log-jsonl.hpp"
#include "pipeline.hpp"
#include "shutdown.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>
#include <domain/error.hpp>
#include <domain/ids.hpp>

#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]]
    auto dpiDeclarationName(uf::DpiDeclaration declaration) noexcept -> std::string_view
    {
        switch (declaration)
        {
        case uf::DpiDeclaration::Declared: return "Declared";
        case uf::DpiDeclaration::AlreadyDeclared: return "AlreadyDeclared";
        }
        return "Unknown";
    }

    [[nodiscard]]
    auto logIntegrity(
        uf::m0_demo::JsonlLog& log,
        std::string_view role,
        std::optional<uf::m0_demo::IntegrityLevel> integrity
    ) -> uf::Status
    {
        if (integrity)
        {
            return log.write(
                uf::m0_demo::LogLine{"setup", "integrity"}
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
            uf::m0_demo::LogLine{"setup", "integrity"}
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
        uf::m0_demo::Args args,
        uf::m0_demo::JsonlLog& log
    ) -> uf::Result<uf::m0_demo::RunSummary>
    {
        UF_TRY_VALUE(dpiDeclaration, uf::ensurePerMonitorAwareV2());
        UF_TRY(
            log.write(
                uf::m0_demo::LogLine{"setup", "dpi_declared"}
                    .outcome("ok")
                    .detail(std::string{dpiDeclarationName(dpiDeclaration)})
            )
        );

        UF_TRY(uf::m0_demo::installConsoleControlHandler());
        UF_TRY(
            logIntegrity(
                log,
                "automation",
                uf::m0_demo::currentProcessIntegrity()
            )
        );

        UF_TRY_VALUE(candidates, uf::enumerateCandidates());
        auto const selector = uf::m0_demo::buildSelector(args.m_selector);
        UF_TRY_VALUE(resolved, uf::resolveTarget(candidates, selector));
        auto const windowHandle = resolved.windowHandle();
        auto const generation = resolved.currentGeneration();
        auto const client = resolved.clientSize();
        auto const process = resolved.identity().process();
        UF_TRY(
            log.write(
                uf::m0_demo::LogLine{"discovery", "resolved"}
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
                uf::m0_demo::processIntegrity(process)
            )
        );
        UF_TRY(uf::m0_demo::ensureClientAreaUsable(client));
        UF_TRY_VALUE(origin, uf::m0_demo::clientOriginDesktop(windowHandle));
        UF_TRY_VALUE(
            geometry,
            uf::ClientGeometry::create(
                origin,
                static_cast<float>(client.width()),
                static_cast<float>(client.height())
            )
        );

        UF_TRY_VALUE(
            homeTemplate,
            uf::m0_demo::loadTemplate(
                args.m_homeTemplate,
                "home",
                args.m_homeRoi
            )
        );
        UF_TRY_VALUE(
            resultTemplate,
            uf::m0_demo::loadTemplate(
                args.m_resultTemplate,
                "result",
                args.m_resultRoi
            )
        );
        UF_TRY_VALUE(
            resetTemplate,
            uf::m0_demo::loadTemplate(
                args.m_resetTemplate,
                "reset",
                args.m_resetRoi
            )
        );
        auto const templates = uf::m0_demo::Templates{
            .m_home = std::move(homeTemplate),
            .m_result = std::move(resultTemplate),
            .m_reset = std::move(resetTemplate),
        };
        auto const config = uf::m0_demo::LoopConfig{
            .m_loops = args.m_loops,
            .m_threshold = args.m_threshold,
            .m_maxActionFrameAge = args.m_maxActionFrameAge,
            .m_transitionTimeout = uf::m0_demo::g_defaultTransitionTimeout,
            .m_guardPolicy = uf::m0_demo::GuardPolicy::forMode(args.m_mode),
            .m_clickDelay = args.m_clickDelay,
            .m_seed = args.m_seed,
        };

        auto const sessionId = uf::SessionId{1};
        UF_TRY_VALUE(
            options,
            uf::WgcCaptureOptions::create(args.m_stallTimeout, false)
        );
        UF_TRY_VALUE(
            delivery,
            uf::DeliveryTarget::create(
                windowHandle,
                sessionId,
                generation,
                client.width(),
                client.height()
            )
        );
        UF_TRY_VALUE(
            session,
            uf::WgcCaptureSession::create(
                windowHandle,
                sessionId,
                generation,
                geometry,
                options
            )
        );
        auto const hygiene = session.hygiene();
        UF_TRY(
            log.write(
                uf::m0_demo::LogLine{"setup", "session_created"}
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

        return uf::m0_demo::runPipeline(
            std::move(resolved),
            std::move(session),
            delivery,
            templates,
            config,
            log
        );
    }

    [[nodiscard]]
    auto run(
        std::span<std::string const> raw
    ) -> uf::Result<uf::m0_demo::RunSummary>
    {
        UF_TRY_VALUE(args, uf::m0_demo::parseArguments(raw));
        UF_TRY_VALUE(log, uf::m0_demo::JsonlLog::create(args.m_log));

        auto outcome = runWithLog(std::move(args), log);
        auto terminalWrite = outcome
            ? uf::ok()
            : log.write(
                uf::m0_demo::LogLine{"run", "fatal"}
                    .outcome("error")
                    .detail(uf::m0_demo::formatAutomationError(outcome.error()))
            );
        auto flush = log.flush();
        if (!outcome)
        {
            return std::unexpected{std::move(outcome).error()};
        }

        UF_TRY(terminalWrite);
        UF_TRY(flush);
        return *std::move(outcome);
    }
}

auto main(int argumentCount, char const* const* p_arguments) -> int
{
    try
    {
        auto raw = std::vector<std::string>{};
        for (auto index = 1; index < argumentCount; ++index)
        {
            raw.emplace_back(p_arguments[index]);
        }
        auto const outcome = run(raw);
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
        std::cerr << "m0-demo exception: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    catch (...)
    {
        std::cerr << "m0-demo exception: unknown failure\n";
        return EXIT_FAILURE;
    }
}
