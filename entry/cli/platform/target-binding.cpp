#include "target-binding.hpp"

#include "controller-action-sink.hpp"
#include "wgc-frame-source.hpp"
#include "windows-target-geometry.hpp"

#include "../candidate-selection.hpp"

#include <controller/capture.hpp>
#include <controller/discovery.hpp>
#include <controller/dpi.hpp>
#include <controller/input.hpp>
#include <controller/target.hpp>

#include <core/error/result.hpp>

#include <annotation/resource.hpp>

#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <memory>
#include <string_view>
#include <utility>

namespace uf::cli::platform
{
    auto bindTarget(std::string_view selector) -> Result<BoundTarget>
    {
        // 1. Declare per-monitor DPI awareness through the controller.
        UF_TRY(ensurePerMonitorAwareV2());

        // 2. Enumerate, pick by selector substring, and resolve the target.
        UF_TRY_VALUE(candidates, enumerateCandidates());
        UF_TRY_VALUE(chosen, selectCandidate(candidates, selector));
        auto const targetSelector = TargetSelector{}.withWindowHandle(chosen.handle());
        UF_TRY_VALUE(resolved, resolveTarget(candidates, targetSelector));

        auto const client       = resolved.clientSize();
        auto const windowHandle = resolved.windowHandle();
        auto const generation   = resolved.currentGeneration();
        auto const sessionId    = CaptureSessionId{1};

        // 3. Build the live fingerprint from resolved geometry and target DPI. A
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

        // 4. Create the capture session from resolved geometry.
        UF_TRY_VALUE(origin, clientOriginDesktop(windowHandle));
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
            .frameSource     = std::make_unique<WgcFrameSource>(std::move(session)),
            .actionSink      = std::make_unique<ControllerActionSink>(delivery),
            .liveFingerprint = liveFingerprint,
        };
    }
}
