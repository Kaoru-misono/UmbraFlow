#pragma once

#include "trace.hpp"

#include <core/error/result.hpp>

#include <domain/detection.hpp>
#include <domain/frame.hpp>
#include <domain/space.hpp>

namespace uf::engine
{
    // A port over ONE bound capture target. Modeled on the surface of the
    // Windows WgcCaptureSession so the platform adapter is a thin wrapper that
    // forwards capture() and revalidates the bound target instance.
    //
    // Seams:
    //  - P3 second platform: a non-Windows adapter implements the same two
    //    methods; nothing above this port is platform-aware.
    //  - Tests: a fake replays a fixed vector<Frame> for CI without a live
    //    desktop, so the engine can be exercised deterministically.
    class IFrameSource
    {
    public:
        IFrameSource() = default;

        IFrameSource(IFrameSource const&) = delete;
        IFrameSource(IFrameSource&&) = delete;
        auto operator=(IFrameSource const&) -> IFrameSource& = delete;
        auto operator=(IFrameSource&&) -> IFrameSource& = delete;

        virtual ~IFrameSource() = default;

        [[nodiscard]] virtual auto capture() -> Result<Frame> = 0;
        [[nodiscard]] virtual auto validateTargetInstance() -> Status = 0;
    };

    // A port that delivers a single background click to the bound target. The
    // engine has already authorized the coordinate (layer 1) by the time click()
    // is called, but that authorization is only the first of two checks. The
    // implementation MUST also forward the lease to the delivery layer so the
    // controller's D0 injection-layer fencing -- frameId, targetGeneration, and
    // age revalidation performed at delivery time -- stays in the loop as layer 2.
    // Dropping the lease would silently remove that security-reviewed second
    // check. The implementation MUST additionally revalidate the target identity
    // before posting and MUST deliver strictly in the background: it never steals
    // focus and never activates the target window.
    class IActionSink
    {
    public:
        IActionSink() = default;

        IActionSink(IActionSink const&) = delete;
        IActionSink(IActionSink&&) = delete;
        auto operator=(IActionSink const&) -> IActionSink& = delete;
        auto operator=(IActionSink&&) -> IActionSink& = delete;

        virtual ~IActionSink() = default;

        [[nodiscard]]
        virtual auto click(
            Point<ClientSpace> point,
            ObservationLease const& lease
        ) -> Status = 0;
    };

    // A port that records one trace event. Traceability is a load-bearing
    // constraint, so an emit failure is an error rather than a best-effort
    // side effect (D4): the engine emits at the throw-instant, before any caller
    // can swallow the failure it describes.
    class ITraceSink
    {
    public:
        ITraceSink() = default;

        ITraceSink(ITraceSink const&) = delete;
        ITraceSink(ITraceSink&&) = delete;
        auto operator=(ITraceSink const&) -> ITraceSink& = delete;
        auto operator=(ITraceSink&&) -> ITraceSink& = delete;

        virtual ~ITraceSink() = default;

        [[nodiscard]] virtual auto emit(TraceEvent const& event) -> Status = 0;
    };
}
