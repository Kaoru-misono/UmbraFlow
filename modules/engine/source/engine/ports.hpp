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
    // forwards capture() and revalidates the bound target instance. Nothing above
    // this port is platform-aware; a test fake replays a fixed vector<Frame> for
    // CI without a live desktop.
    class IFrameSource
    {
    public:
        // The bound on one capture call -- the one engine operation that can
        // block on an external producer, where without a budget an adapter
        // waiting on a compositor decides for itself how long a caller waits and
        // a cancelled run stays blocked in a frame pool.
        //
        // An implementation MUST honour both members: return at the deadline, and
        // promptly once the stop is requested. The deadline is absolute rather
        // than a duration so a caller that already spent part of its budget
        // cannot silently renew it, and carries no default so every construction
        // site states the bound it imposes.
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

    // A port that delivers one background input to the bound target: a click, a
    // keystroke, a wheel scroll, a long press, or a pointer move. Each verb
    // states its own authorization contract below; they do not share one,
    // because what the engine has already authorized differs between a verb that
    // names a coordinate and one that does not.
    //
    // Owed by every verb regardless. For a coordinate-bearing verb the engine has
    // authorized the point (layer 1) by the time the call is made, but that is
    // only the first of two checks: the implementation MUST forward the lease to
    // the delivery layer so the controller's D0 injection-layer fencing --
    // frameId, targetGeneration, and age revalidation at delivery time -- stays
    // in the loop as layer 2. Every implementation MUST also revalidate the
    // target identity before posting and MUST deliver strictly in the background,
    // never stealing focus and never activating the target window.
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
        // It takes a TargetGeneration where click() takes a lease, which is the
        // whole authorization difference between the two verbs: a lease fences a
        // COORDINATE, whose frameId and age exist because a click point silently
        // means something else once the layout moved. A keystroke names no point.
        // What must still hold is that it reaches the target instance the
        // observation came from, which is what the generation carries and which
        // the implementation MUST forward so the controller's revalidation runs
        // at post time.
        [[nodiscard]]
        virtual auto pressKey(
            KeyName key,
            TargetGeneration actionGeneration
        ) -> Status = 0;

        // Delivers one wheel scroll of `notches` detents to the bound target,
        // positive away from the operator and negative toward them.
        //
        // It takes a lease where pressKey() takes a bare generation, and that is
        // not the authorization difference it looks like: a scroll shares the
        // keystroke's contract above this port, naming no coordinate and getting
        // no fingerprint or lease-age check. What it does not share is DELIVERY.
        // A wheel message is posted at a position on every target this project
        // drives, so an implementation must choose one and be able to refuse a
        // position it can no longer aim at. The lease is that material, and
        // dropping it would remove the controller's D0 fence exactly as dropping
        // it from click() would.
        //
        // WHICH position a scroll is aimed at is deliberately open
        // (docs/plans/2026-08-01-three-layers-and-agent-operator.md section 9 item
        // 5). Until it is settled an implementation aims at the bound target
        // itself; when it is, the point arrives here as a parameter and nothing
        // about the lease changes.
        //
        // `notches` crosses as a plain count because its bound is not
        // platform-neutral: Windows carries the delta in a signed 16-bit word, so
        // the implementation owns the unit conversion, the refusal of a count that
        // does not fit that word, and the refusal of zero.
        [[nodiscard]]
        virtual auto scroll(
            int32 notches,
            ObservationLease const& lease
        ) -> Status = 0;

        // Delivers one long press at `point`: the button goes down, stays down
        // for `hold`, and comes back up before this returns.
        //
        // The port exposes this and not pointerDown/pointerUp, though the
        // controller has had all three since D0, because a click's authorization
        // is a statement about ONE instant and a long press is still that: press,
        // hold and release begin and end inside this call, so nothing above ever
        // holds a half-pressed target. A bare pointerDown would hand a caller a
        // hold spanning many frames, and the model has no answer yet to who
        // guarantees the matching release, what a cancel mid-hold does, or what a
        // frame captured during one means.
        //
        // The hold is the CALLER'S with no default here or above: how long a
        // target needs a button held is a fact about that target, and a duration
        // the caller cannot see is one it never chose.
        //
        // It takes a lease for click()'s reason and not for scroll()'s -- this
        // verb names a coordinate measured off a frame, so the lease is
        // authorization and not merely delivery material. The implementation MUST
        // also leave the button released on every exit path including a failed
        // one: a verb whose safety argument is that it strands no held state
        // cannot strand held state when it fails.
        [[nodiscard]]
        virtual auto longPress(
            Point<ClientSpace> point,
            MonotonicInstant::Duration hold,
            ObservationLease const& lease
        ) -> Status = 0;

        // Moves the pointer to `point` on the bound target, pressing nothing.
        //
        // It exists because a posted wheel does not scroll what the message
        // points at: a target tracks which container is hovered from pointer
        // messages and scrolls whatever it already believes is hovered, so a
        // wheel with no pointer message before it moves nothing at all
        // (docs/pitfalls/capture-and-target-selection.md). This is the verb that
        // says "the pointer is over this region" without also pressing it.
        //
        // It takes a lease for click()'s reason and not for scroll()'s: the point
        // was measured off a frame, so the lease is authorization here rather than
        // only delivery material.
        //
        // The implementation MUST NOT press, release, or hold any button. That is
        // the entire difference from click(), and it is what lets the engine admit
        // this verb where the page model authorises no activation.
        [[nodiscard]]
        virtual auto movePointer(
            Point<ClientSpace> point,
            ObservationLease const& lease
        ) -> Status = 0;
    };
}
