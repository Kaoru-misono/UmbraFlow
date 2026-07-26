#include "run.hpp"

#include "candidate-selection.hpp"
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
#include <memory>
#include <utility>

namespace uf::cli
{
    auto runProduct(RunArgs const& args) -> Result<RunReport>
    {
        // Load the project and resolve the page and action names before touching
        // the desktop, so a bad project or unknown name fails fast without binding
        // any target.
        UF_TRY_VALUE(loaded, engine::loadRuntimeProject(args.project));
        UF_TRY_VALUE(
            pageId,
            resolvePageName(loaded.runtime.manifest().catalog(), args.page)
        );
        UF_TRY_VALUE(
            actionId,
            resolveActionName(loaded.runtime.manifest().catalog(), args.action)
        );

        // 1. Declare per-monitor DPI awareness through the controller.
        UF_TRY(ensurePerMonitorAwareV2());

        // 2. Install Ctrl-C cancellation feeding the engine's stop token.
        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // 3. Enumerate, pick by selector substring, and resolve the target.
        UF_TRY_VALUE(candidates, enumerateCandidates());
        UF_TRY_VALUE(chosen, selectCandidate(candidates, args.selector));
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
        UF_TRY_VALUE(traceSink, FileTraceSink::create(args.trace));
        auto frameSource = std::make_unique<platform::WgcFrameSource>(
            std::move(session)
        );
        auto actionSink = std::make_unique<platform::ControllerActionSink>(delivery);

        // 8. Build the session and drive the smoke flow.
        auto config = engine::EngineSessionConfig{
            .liveFingerprint         = liveFingerprint,
            .maximumPixelComparisons = args.budget,
            .recognitionTimeout      = args.recognitionTimeout,
            .maxActionFrameAge       = args.maxFrameAge,
            .cancellation            = cancellation.token(),
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
            engineSession.waitForPage(pageId, args.timeout, args.pollInterval)
        );
        UF_TRY_VALUE(maybeAction, pageWait.observation.findAction(actionId));

        auto report = RunReport{
            .pageName   = args.page,
            .actionName = args.action,
            .tracePath  = args.trace.string(),
        };
        if (!maybeAction)
        {
            report.actionDelivered = false;
            return report;
        }

        UF_TRY_VALUE(
            receipt,
            engineSession.act(
                std::move(pageWait.observation),
                pageWait.page,
                *maybeAction
            )
        );
        report.actionDelivered = true;
        report.clickClientX    = receipt.clickPoint.x();
        report.clickClientY    = receipt.clickPoint.y();
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
