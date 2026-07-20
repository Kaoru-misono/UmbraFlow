#pragma once

#include <controller/input.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <domain/detection.hpp>
#include <domain/space.hpp>

#include <span>
#include <string>

namespace uf::m0_demo
{
    auto clearInputAgentCommandAudit(AuditLog& audit) noexcept -> void;

    [[nodiscard]]
    auto validateInputAgentClick(
        DeliveryTarget const& target,
        ObservationLease lease,
        Point<ClientSpace> point,
        MonotonicInstant now
    ) -> Status;

    [[nodiscard]]
    auto runInputAgent(
        std::span<std::string const> raw
    ) -> Status;
}
