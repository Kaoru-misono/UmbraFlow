#include "run.hpp"

#include "file-trace-sink.hpp"
#include "name-resolution.hpp"
#include "platform/controller-action-sink.hpp"
#include "platform/wgc-frame-source.hpp"
#include "platform/windows-console-cancellation.hpp"
#include "platform/windows-target-geometry.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>
#include <core/error/result.hpp>

#include <annotation/catalog.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>
#include <engine/runtime-loader.hpp>
#include <engine/session.hpp>

#include <cstddef>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::cli
{
    namespace
    {
        // Picks the single candidate whose title contains the selector substring.
        // Zero or multiple matches are a resolution failure the caller must refine,
        // so the flow never guesses which window the operator meant.
        [[nodiscard]]
        auto selectCandidate(
            std::span<TargetCandidate const> candidates,
            std::string_view selector
        ) -> Result<TargetCandidate>
        {
            auto matches = std::vector<TargetCandidate const*>{};
            for (auto const& candidate : candidates)
            {
                if (candidate.title().find(selector) != std::string::npos)
                {
                    matches.emplace_back(&candidate);
                }
            }

            if (matches.empty())
            {
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    std::format("no window title contains \"{}\"", selector)
                );
            }
            if (matches.size() > 1U)
            {
                auto titles = std::string{};
                for (auto const* p_match : matches)
                {
                    if (!titles.empty())
                    {
                        titles += ", ";
                    }
                    titles += '"';
                    titles += p_match->title();
                    titles += '"';
                }
                return fail(
                    AutomationErrorKind::TargetUnavailable,
                    std::format(
                        "selector \"{}\" matches {} windows; refine it: {}",
                        selector,
                        matches.size(),
                        titles
                    )
                );
            }

            return *matches.front();
        }
    }

    auto runProduct(RunArgs const& args) -> Result<RunReport>
    {
        // Load the project and resolve the page and action names before touching
        // the desktop, so a bad project or unknown name fails fast without binding
        // any target.
        UF_TRY_VALUE(loaded, engine::loadRuntimeProject(args.m_project));
        UF_TRY_VALUE(
            pageId,
            resolvePageName(loaded.m_runtime.manifest().catalog(), args.m_page)
        );
        UF_TRY_VALUE(
            actionId,
            resolveActionName(loaded.m_runtime.manifest().catalog(), args.m_action)
        );

        // 1. Declare per-monitor DPI awareness through the controller.
        UF_TRY(ensurePerMonitorAwareV2());

        // 2. Install Ctrl-C cancellation feeding the engine's stop token.
        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // 3. Enumerate, pick by selector substring, and resolve the target.
        UF_TRY_VALUE(candidates, enumerateCandidates());
        UF_TRY_VALUE(chosen, selectCandidate(candidates, args.m_selector));
        auto const selector = TargetSelector{}.withWindowHandle(chosen.handle());
        UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));

        auto const client       = resolved.clientSize();
        auto const windowHandle = resolved.windowHandle();
        auto const generation   = resolved.currentGeneration();
        auto const sessionId    = SessionId{1};

        // 4. Build the live fingerprint from resolved geometry and target DPI. A
        // mismatch against the manifest fingerprint makes the engine fail closed.
        auto const dpi = chosen.dpi().value();
        UF_TRY_VALUE(
            liveFingerprint,
            annotation::ProjectFingerprint::create(
                client.width(),
                client.height(),
                dpi,
                dpi
            )
        );

        // 5. Create the capture session from resolved geometry.
        UF_TRY_VALUE(origin, platform::clientOriginDesktop(windowHandle));
        UF_TRY_VALUE(
            geometry,
            ClientGeometry::create(
                origin,
                static_cast<float>(client.width()),
                static_cast<float>(client.height())
            )
        );
        UF_TRY_VALUE(
            session,
            WgcCaptureSession::create(
                windowHandle,
                sessionId,
                generation,
                geometry
            )
        );

        // 6. Create the delivery target for background input.
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

        // 7. Wire the adapters over the resolved capabilities.
        UF_TRY_VALUE(traceSink, FileTraceSink::create(args.m_trace));
        auto frameSource = std::make_unique<platform::WgcFrameSource>(
            std::move(session)
        );
        auto actionSink = std::make_unique<platform::ControllerActionSink>(delivery);

        // 8. Build the session and drive the smoke flow.
        auto config = engine::EngineSessionConfig{
            .m_liveFingerprint         = liveFingerprint,
            .m_maximumPixelComparisons = args.m_budget,
            .m_recognitionTimeout      = args.m_recognitionTimeout,
            .m_maxActionFrameAge       = args.m_maxFrameAge,
            .m_cancellation            = cancellation.token(),
        };
        UF_TRY_VALUE(
            engineSession,
            engine::EngineSession::create(
                std::move(loaded),
                std::move(frameSource),
                std::move(actionSink),
                std::move(traceSink),
                std::move(config)
            )
        );

        UF_TRY_VALUE(
            pageWait,
            engineSession.waitForPage(pageId, args.m_timeout, args.m_pollInterval)
        );
        UF_TRY_VALUE(maybeAction, pageWait.m_observation.findAction(actionId));

        auto report = RunReport{
            .m_pageName   = args.m_page,
            .m_actionName = args.m_action,
            .m_tracePath  = args.m_trace.string(),
        };
        if (!maybeAction)
        {
            report.m_actionDelivered = false;
            return report;
        }

        UF_TRY_VALUE(
            receipt,
            engineSession.act(
                std::move(pageWait.m_observation),
                pageWait.m_page,
                *maybeAction
            )
        );
        report.m_actionDelivered = true;
        report.m_clickClientX    = receipt.m_clickPoint.x();
        report.m_clickClientY    = receipt.m_clickPoint.y();
        return report;
    }

    // Reads the process-lifetime console cancellation source, which outlives the
    // ConsoleCancellation registration installed during runProduct, so a Ctrl-C
    // remains observable at the exit-code boundary after the run has returned.
    auto runCancellationRequested() noexcept -> bool
    {
        return platform::ConsoleCancellation::stopRequested();
    }
}
