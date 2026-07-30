#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>

#include <domain/detection.hpp>
#include <domain/frame.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>

#include <stop_token>

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
        // The bound on one capture call. It is nested because nothing names it
        // except capture(), and it exists because a capture is the one engine
        // operation that can block on an external producer: without it an
        // adapter waiting on a compositor decides for itself how long a caller
        // waits, and a cancelled run stays blocked in a frame pool.
        //
        // Both members are load-bearing and an implementation MUST honour both.
        // The deadline is absolute rather than a duration so a caller that
        // already spent part of its budget cannot silently renew it, and it has
        // no default because MonotonicInstant refuses to invent one -- every
        // construction site states the bound it is imposing. Returning at the
        // deadline, or promptly once the stop is requested, is the contract; a
        // capture that outlives either is a defect in that adapter.
        struct CaptureBudget final
        {
            MonotonicInstant deadline;
            std::stop_token  cancellation{};
        };

        IFrameSource() = default;

        IFrameSource(IFrameSource const&) = delete;
        IFrameSource(IFrameSource&&) = delete;
        auto operator=(IFrameSource const&) -> IFrameSource& = delete;
        auto operator=(IFrameSource&&) -> IFrameSource& = delete;

        virtual ~IFrameSource() = default;

        [[nodiscard]]
        virtual auto capture(CaptureBudget const& budget) -> Result<Frame> = 0;

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

        // Delivers one press-and-release of `key` to the bound target.
        //
        // It takes a TargetGeneration where click() takes a lease, and the
        // difference is the whole authorization difference between the two verbs.
        // A lease fences a COORDINATE: its frameId and age exist because a click
        // point silently means something else once the layout moved. A keystroke
        // names no point, so there is no rect whose position could have gone
        // stale, and there is nothing for a frame age to protect. What must still
        // hold is that the keystroke reaches the target instance the observation
        // came from, which is what the generation carries.
        //
        // The implementation MUST forward that generation to the delivery layer so
        // the controller's revalidation runs at post time, MUST deliver strictly in
        // the background, and MUST never steal focus or activate the target window.
        [[nodiscard]]
        virtual auto pressKey(
            KeyName key,
            TargetGeneration actionGeneration
        ) -> Status = 0;
    };
}
