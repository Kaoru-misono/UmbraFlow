#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>

#include <domain/detection.hpp>
#include <domain/space.hpp>
#include <engine/ports.hpp>

namespace uf::cli::platform
{
    // Adapts controller background input to the engine ActionSink port. It owns
    // the delivery target and the per-target input bookkeeping (held inputs and
    // the audit log) that controller::click threads through. The observation
    // lease is forwarded into controller::click so the injection-layer fence
    // (frameId, targetGeneration, age) re-runs at delivery time as layer 2.
    class ControllerActionSink final : public engine::ActionSink
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
    };
}
