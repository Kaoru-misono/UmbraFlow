#include "run.hpp"

#include "candidate-selection.hpp"
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

#include <annotation/resource.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <task/task-host.hpp>

#include <memory>
#include <utility>

namespace uf::cli
{
    namespace
    {
        // The ports one run drives. Everything here is desktop binding:
        // enumerating windows, resolving the target, opening the capture session
        // and the delivery target, and wrapping each in the engine port its
        // adapter implements. This is the whole of what the CLI still contributes
        // to a run -- the run lifecycle itself lives in task::TaskHost.
        struct BoundTarget final
        {
            std::unique_ptr<engine::IFrameSource> frameSource;
            std::unique_ptr<engine::IActionSink>  actionSink;

            annotation::ProjectFingerprint liveFingerprint;
        };

        // Declares DPI awareness, resolves the target by selector substring,
        // builds the live fingerprint from the resolved geometry, and wires the
        // capture session and delivery target into the two engine ports.
        [[nodiscard]]
        auto bindTarget(RunArgs const& args) -> Result<BoundTarget>
        {
            // 1. Declare per-monitor DPI awareness through the controller.
            UF_TRY(ensurePerMonitorAwareV2());

            // 2. Enumerate, pick by selector substring, and resolve the target.
            UF_TRY_VALUE(candidates, enumerateCandidates());
            UF_TRY_VALUE(chosen, selectCandidate(candidates, args.selector));
            auto const selector = TargetSelector{}.withWindowHandle(chosen.handle());
            UF_TRY_VALUE(resolved, resolveTarget(candidates, selector));

            auto const client       = resolved.clientSize();
            auto const windowHandle = resolved.windowHandle();
            auto const generation   = resolved.currentGeneration();
            auto const sessionId    = CaptureSessionId{1};

            // 3. Build the live fingerprint from resolved geometry and target DPI.
            // A mismatch against the manifest fingerprint makes the engine fail
            // closed.
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

            // 4. Create the capture session from resolved geometry.
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

            // 5. Create the delivery target for background input.
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

            return BoundTarget{
                .frameSource = std::make_unique<platform::WgcFrameSource>(
                    std::move(session)
                ),
                .actionSink = std::make_unique<platform::ControllerActionSink>(
                    delivery
                ),
                .liveFingerprint = liveFingerprint,
            };
        }
    }

    auto runProduct(RunArgs const& args) -> Result<task::TaskRunReport>
    {
        // Install Ctrl-C cancellation before anything can block. The registration
        // is held here, ahead of every later local, so it stays installed for the
        // whole run and is removed last; its token is what lets the generation --
        // and through it the engine and the VM interrupt -- observe a stop.
        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // Load the project before touching the desktop, so a bad project path or
        // a corrupt manifest fails without declaring DPI awareness, enumerating
        // windows, or opening any capture resource.
        auto host = task::TaskHost{};
        UF_TRY_VALUE(
            generation,
            host.loadProject(
                args.project,
                task::TaskHostConfig{
                    .externalCancellation = cancellation.token(),
                }
            )
        );

        UF_TRY_VALUE(bound, bindTarget(args));

        return host.startTask(
            generation,
            args.task,
            task::TaskRunConfig{
                .frameSource             = std::move(bound.frameSource),
                .actionSink              = std::move(bound.actionSink),
                .liveFingerprint         = bound.liveFingerprint,
                .maximumPixelComparisons = args.budget,
                .recognitionTimeout      = args.recognitionTimeout,
                .maxActionFrameAge       = args.maxFrameAge,
                .defaultWaitTimeout      = args.timeout,
                .defaultWaitPollInterval = args.pollInterval,
                .tracePath               = args.trace,
            }
        );
    }

    // Reads the process-lifetime console cancellation source, which outlives the
    // ConsoleCancellation registration installed during a run, so a Ctrl-C remains
    // observable at the exit-code boundary after the run has returned.
    auto runCancellationRequested() noexcept -> bool
    {
        return platform::ConsoleCancellation::stopRequested();
    }
}
