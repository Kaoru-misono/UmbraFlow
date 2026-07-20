#pragma once

#include "controller/input.hpp"

#include <core/error/result.hpp>

namespace uf::controller_detail
{
    [[nodiscard]]
    auto checkPointerPreconditions(
        ObservationLease lease,
        SessionId currentSession,
        TargetGeneration currentGeneration,
        MonotonicInstant now,
        Point<ClientSpace> point,
        std::uint32_t clientWidth,
        std::uint32_t clientHeight
    ) -> Result<ClientPixel>;

    [[nodiscard]]
    auto checkKeyboardPreconditions(
        TargetGeneration actionGeneration,
        TargetGeneration currentGeneration
    ) -> Status;

    [[nodiscard]] auto windowIsAlive(WindowHandle windowHandle) noexcept -> bool;
    [[nodiscard]] auto ensureWindowAlive(WindowHandle windowHandle) -> Status;

    [[nodiscard]]
    auto ensureSameDeliveryIdentity(
        DeliveryTarget const& original,
        DeliveryTarget const& refreshed
    ) -> Status;
}
