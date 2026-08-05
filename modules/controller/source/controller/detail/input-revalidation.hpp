#pragma once

#include "controller/input.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

namespace uf::controller_detail
{
    // Everything a pointer message must satisfy EXCEPT observation freshness: the
    // lease still names the live capture session and target generation, and the
    // point is inside the client area and encodable as a mouse coordinate.
    //
    // Split out for the inside of a gesture. A drag holds the button down for as
    // long as the caller asked, and the gesture's own effect is what makes the
    // observation stale, so re-fencing freshness per waypoint would refuse every
    // drag slower than the lease. What must not change under a held button is
    // WHICH WINDOW the moves land in, and that is exactly what stays here.
    [[nodiscard]]
    auto checkPointerTarget(
        ObservationLease lease,
        CaptureSessionId currentSession,
        TargetGeneration currentGeneration,
        Point<ClientSpace> point,
        uint32 clientWidth,
        uint32 clientHeight
    ) -> Result<ClientPixel>;

    // checkPointerTarget plus the freshness fence, for the moment an action is
    // authorised against an observation.
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
