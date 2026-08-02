#pragma once

#include <controller/input.hpp>
#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>
#include <engine/ports.hpp>

#include <string_view>

namespace uf::cli::platform
{
    // Adapts controller background input to the engine IActionSink port, owning the
    // per-target input bookkeeping controller::click threads through. The
    // observation lease is forwarded so the injection-layer fence (frameId,
    // targetGeneration, age) re-runs at delivery time as layer 2.
    class ControllerActionSink final : public engine::IActionSink
    {
        DeliveryTarget m_target;
        HeldInputs     m_held{};
        AuditLog       m_audit{};

        // Drains whatever the failed verb left held and returns the verb's own
        // error, with a note appended when the drain itself failed. One function for
        // all three verbs that can leave a button or key down: `what` names the
        // verb, and nothing else differs.
        [[nodiscard]]
        auto drainAfterFailure(Error error, std::string_view what) -> Status;

    public:
        explicit ControllerActionSink(DeliveryTarget target) noexcept
            : m_target{target}
        {
        }

        [[nodiscard]]
        auto click(
            Point<ClientSpace> point,
            ObservationLease const& lease
        ) -> Status override;

        // Delivers one press-and-release through controller::keyPress, the same
        // deliver -> postInputMessage -> PostMessageW route click() takes. It
        // forwards the observation's target generation where click() forwards a
        // lease, because a keystroke has no coordinate whose frame identity or age
        // could matter. Resolving the name cannot fail -- domain::KeyName admits only
        // names controller::KeyInput maps.
        [[nodiscard]]
        auto pressKey(
            KeyName key,
            TargetGeneration actionGeneration
        ) -> Status override;

        // Turns the port's detent count into the controller's WheelDelta -- where
        // "not zero" and "small enough for the word the wheel message encodes it in"
        // are decided -- and posts it through controller::scroll, forwarding the
        // lease so a click's injection-layer fence runs here too. It supplies the
        // position controller::scroll needs, since the port's verb names none; see
        // the definition for which position and why.
        [[nodiscard]]
        auto scroll(
            int32 notches,
            ObservationLease const& lease
        ) -> Status override;

        // Posts the press, holds it, re-reads the bound window, and posts the
        // release through controller::longPress -- the same route click() takes,
        // with the same lease forwarded. It supplies the refresh-target callback
        // controller::longPress requires; see the definition for what that callback
        // can honestly re-read here.
        [[nodiscard]]
        auto longPress(
            Point<ClientSpace> point,
            MonotonicInstant::Duration hold,
            ObservationLease const& lease
        ) -> Status override;
    };
}
