#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/detection.hpp>
#include <domain/ids.hpp>
#include <domain/key.hpp>
#include <domain/space.hpp>
#include <engine/ports.hpp>

namespace uf::cli::platform
{
    // Adapts controller background input to the engine IActionSink port. It owns
    // the delivery target and the per-target input bookkeeping (held inputs and
    // the audit log) that controller::click threads through. The observation
    // lease is forwarded into controller::click so the injection-layer fence
    // (frameId, targetGeneration, age) re-runs at delivery time as layer 2.
    class ControllerActionSink final : public engine::IActionSink
    {
        DeliveryTarget m_target;
        HeldInputs     m_held{};
        AuditLog       m_audit{};

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

        // Resolves the key's printed name to a virtual key and delivers one
        // press-and-release through controller::keyPress, which is the same
        // deliver -> postInputMessage -> PostMessageW route click() takes.
        //
        // It forwards the observation's target generation where click() forwards a
        // lease, because that is what the delivery layer fences a keystroke on: there
        // is no coordinate here whose frame identity or age could matter, and the
        // controller's keyPress accordingly takes the generation alone. Resolving
        // cannot fail -- domain::KeyName admits only names controller::KeyInput maps.
        [[nodiscard]]
        auto pressKey(
            KeyName key,
            TargetGeneration actionGeneration
        ) -> Status override;

        // Turns the port's detent count into the controller's WheelDelta -- which
        // is where "not zero" and "small enough for the word the wheel message
        // encodes it in" are decided -- and posts it through controller::scroll,
        // forwarding the lease so the same injection-layer fence a click gets runs
        // at delivery time.
        //
        // It supplies the position controller::scroll needs, because the port's
        // verb names none: see the definition for which position and why that
        // choice leaves the anchoring question open rather than answering it.
        [[nodiscard]]
        auto scroll(
            int32 notches,
            ObservationLease const& lease
        ) -> Status override;
    };
}
