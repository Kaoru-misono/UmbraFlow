#pragma once

#include "controller/input.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

namespace uf::controller_detail
{
    [[nodiscard]]
    auto checkPointerPreconditions(
        ObservationLease lease,
        CaptureSessionId currentSession,
        TargetGeneration currentGeneration,
        MonotonicInstant now,
        Point<ClientSpace> point,
        uint32 clientWidth,
        uint32 clientHeight
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
