#include "detail/input-revalidation.hpp"

#include "platform/windows-controller.hpp"

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <cmath>
#include <format>
#include <limits>

namespace uf::controller_detail
{
    namespace
    {
        [[nodiscard]]
        auto describeDeliveryIdentity(DeliveryTarget const& target) -> std::string
        {
            return std::format(
                "DeliveryIdentity {{ hwnd: {}, session_id: CaptureSessionId({}), generation: TargetGeneration({}) }}",
                static_cast<uintptr>(target.windowHandle().value()),
                target.sessionId().value(),
                target.generation().value()
            );
        }

        // Written once and called from both entry points, so the session clause
        // reads identically whether or not freshness is being fenced. The
        // fenced entry point reports it BEFORE expiry: a lease from another
        // capture session is the more specific complaint of the two.
        [[nodiscard]]
        auto checkLeaseSession(ObservationLease lease, CaptureSessionId currentSession) -> Status
        {
            if (lease.sessionId() != currentSession)
            {
                return fail(
                    AutomationErrorKind::StaleObservation,
                    std::format(
                        "lease session CaptureSessionId({}) != current CaptureSessionId({})",
                        lease.sessionId().value(),
                        currentSession.value()
                    )
                );
            }
            return ok();
        }
    }
}

namespace uf::controller_detail
{
    auto checkPointerPreconditions(
        ObservationLease lease,
        CaptureSessionId currentSession,
        TargetGeneration currentGeneration,
        MonotonicInstant now,
        Point<ClientSpace> point,
        uint32 clientWidth,
        uint32 clientHeight
    ) -> Result<ClientPixel>
    {
        UF_TRY(checkLeaseSession(lease, currentSession));
        if (lease.isExpired(now))
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "lease expired: observation older than max action frame age"
            );
        }
        return checkPointerTarget(
            lease,
            currentSession,
            currentGeneration,
            point,
            clientWidth,
            clientHeight
        );
    }

    auto checkPointerTarget(
        ObservationLease lease,
        CaptureSessionId currentSession,
        TargetGeneration currentGeneration,
        Point<ClientSpace> point,
        uint32 clientWidth,
        uint32 clientHeight
    ) -> Result<ClientPixel>
    {
        UF_TRY(checkLeaseSession(lease, currentSession));
        if (lease.targetGeneration() != currentGeneration)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                std::format(
                    "lease generation TargetGeneration({}) != current TargetGeneration({})",
                    lease.targetGeneration().value(),
                    currentGeneration.value()
                )
            );
        }
        if (!std::isfinite(point.x()) || !std::isfinite(point.y()))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format("non-finite client point ({}, {})", point.x(), point.y())
            );
        }

        auto const x = static_cast<double>(point.x());
        auto const y = static_cast<double>(point.y());
        auto const width = static_cast<double>(clientWidth);
        auto const height = static_cast<double>(clientHeight);
        if (x < 0.0 || y < 0.0 || x >= width || y >= height)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client point ({}, {}) outside client area {}x{}",
                    point.x(),
                    point.y(),
                    clientWidth,
                    clientHeight
                )
            );
        }

        auto constexpr coordinateLimit = (
            static_cast<double>(std::numeric_limits<int16>::max()) + 1.0
        );
        if (x >= coordinateLimit || y >= coordinateLimit)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client point ({}, {}) cannot be encoded as signed-16-bit mouse coordinates",
                    point.x(),
                    point.y()
                )
            );
        }

        auto const pixelX = static_cast<int32>(std::floor(point.x()));
        auto const pixelY = static_cast<int32>(std::floor(point.y()));
        return ClientPixel::create(pixelX, pixelY);
    }

    auto checkKeyboardPreconditions(
        TargetGeneration actionGeneration,
        TargetGeneration currentGeneration
    ) -> Status
    {
        if (actionGeneration != currentGeneration)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                std::format(
                    "action generation TargetGeneration({}) != current TargetGeneration({})",
                    actionGeneration.value(),
                    currentGeneration.value()
                )
            );
        }
        return ok();
    }

    auto windowIsAlive(WindowHandle windowHandle) noexcept -> bool
    {
        return controller_platform::windowIsAlive(windowHandle);
    }

    auto ensureWindowAlive(WindowHandle windowHandle) -> Status
    {
        if (windowIsAlive(windowHandle))
        {
            return ok();
        }
        return fail(
            AutomationErrorKind::ActionRejected,
            std::format(
                "window handle {:#x} is no longer a valid window",
                static_cast<uintptr>(windowHandle.value())
            )
        );
    }

    auto ensureSameDeliveryIdentity(
        DeliveryTarget const& original,
        DeliveryTarget const& refreshed
    ) -> Status
    {
        if (
            original.windowHandle() != refreshed.windowHandle()
            || original.sessionId() != refreshed.sessionId()
            || original.generation() != refreshed.generation()
        )
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                std::format(
                    "delivery target changed during held action: original {}, refreshed {}",
                    describeDeliveryIdentity(original),
                    describeDeliveryIdentity(refreshed)
                )
            );
        }
        return ok();
    }
}
