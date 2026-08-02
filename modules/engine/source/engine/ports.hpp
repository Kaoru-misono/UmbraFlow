#pragma once

#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

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

        // Delivers one wheel scroll of `notches` detents to the bound target,
        // positive away from the operator and negative toward them.
        //
        // WHY IT TAKES A LEASE WHERE pressKey() TAKES A BARE GENERATION, and why
        // that is not the authorization difference it looks like. The verb above
        // this port names no coordinate and the engine enforces none: no
        // fingerprint check and no lease age, because there is no rectangle whose
        // meaning either could invalidate. That is the keystroke's contract and a
        // scroll shares it. What a scroll does not share is its DELIVERY: a wheel
        // message is posted at a position on every target this project drives, so
        // an implementation has to choose one and has to be able to refuse a
        // position it can no longer aim at. The lease is that material, and
        // dropping it would remove the controller's D0 injection-layer fence
        // exactly as dropping it from click() would.
        //
        // WHICH position a scroll should be aimed at is deliberately open
        // (docs/plans/2026-08-01-three-layers-and-agent-operator.md section 9 item
        // 5 -- anchoring one to an annotated region). Until that is settled an
        // implementation aims at the bound target itself and no annotated region
        // takes part; when it is settled the point arrives here as a parameter and
        // nothing about the lease changes.
        //
        // `notches` crosses as a plain count because its bound is not
        // platform-neutral: Windows carries the delta in a signed 16-bit word, so
        // the implementation owns the unit conversion, the refusal of a count that
        // does not fit that word, and the refusal of zero -- a wheel message that
        // moves nothing is a mistyped command rather than a no-op worth posting.
        // The implementation MUST forward the lease to the delivery layer, MUST
        // deliver strictly in the background, and MUST never steal focus or
        // activate the target window.
        [[nodiscard]]
        virtual auto scroll(
            int32 notches,
            ObservationLease const& lease
        ) -> Status = 0;

        // Delivers one long press at `point`: the button goes down, stays down
        // for `hold`, and comes back up before this returns.
        //
        // WHY THE PORT HAS THIS AND NOT pointerDown/pointerUp. The controller has
        // had all three since D0, so the choice here is which of them becomes a
        // verb anything above the engine can ask for, and it is decided by the
        // authorization model rather than by convenience. A click is authorized
        // by a hit located on the frame it is delivered to; that is a statement
        // about ONE instant, and a long press is still one instant's worth of
        // authorization because press, hold and release are one act that begins
        // and ends inside this call -- nothing above ever holds a half-pressed
        // target, and no observation is left describing a screen with a button
        // stuck down in it. A bare pointerDown is not that. It would hand a
        // caller a hold spanning many frames, and the model has no answer yet to
        // "who guarantees the matching release" -- not which layer owns it, not
        // what happens when the run is cancelled mid-hold, not what a frame
        // captured during one even means. controller::releaseHeld exists because
        // the platform layer thought that through for ITSELF; the semantics
        // upward have not been decided, and shipping down/up would decide them
        // by accident.
        //
        // The hold is the CALLER'S and has no default here or anywhere above:
        // how long a target needs a button held to treat it as a long press is a
        // fact about that target, and a duration the caller cannot see is one it
        // never chose.
        //
        // It takes a lease for click()'s reason and not for scroll()'s: this verb
        // names a coordinate the caller measured off a frame, so the lease is
        // authorization here and not merely delivery material. The implementation
        // MUST forward it to the delivery layer so the controller's D0 fencing
        // re-runs at post time, MUST deliver strictly in the background, and MUST
        // never steal focus or activate the target window. It MUST also leave the
        // button released on every exit path, including a failed one -- a verb
        // whose whole safety argument is that it strands no held state cannot
        // strand held state when it fails.
        [[nodiscard]]
        virtual auto longPress(
            Point<ClientSpace> point,
            MonotonicInstant::Duration hold,
            ObservationLease const& lease
        ) -> Status = 0;
    };
}
