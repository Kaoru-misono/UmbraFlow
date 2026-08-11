#include "detection.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <utility>

namespace uf
{
    auto clampMaxActionFrameAge(
        MonotonicInstant::Duration requested
    ) noexcept -> MonotonicInstant::Duration
    {
        return std::min(requested, k_defaultMaxActionFrameAge);
    }

    Detection::Detection(
        CaptureSessionId sessionId,
        TargetGeneration targetGeneration,
        FrameId frameId,
        Label label,
        Rect<FrameSpace> rect,
        float confidence
    )
        : m_sessionId{sessionId}
        , m_targetGeneration{targetGeneration}
        , m_frameId{frameId}
        , m_label{std::move(label)}
        , m_rect{rect}
        , m_confidence{confidence}
    {
    }

    auto Detection::sessionId() const noexcept -> CaptureSessionId { return m_sessionId; }
    auto Detection::targetGeneration() const noexcept -> TargetGeneration
    {
        return m_targetGeneration;
    }
    auto Detection::frameId() const noexcept -> FrameId { return m_frameId; }
    auto Detection::label() const noexcept -> Label const& { return m_label; }
    auto Detection::rect() const noexcept -> Rect<FrameSpace> { return m_rect; }
    auto Detection::confidence() const noexcept -> float { return m_confidence; }

    auto ObservationLease::forFrame(
        Frame const& frame,
        MonotonicInstant::Duration maximumAge
    ) -> Result<ObservationLease>
    {
        if (maximumAge < MonotonicInstant::Duration::zero())
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("observation maximum age cannot be negative: {}", maximumAge.count())
            );
        }

        auto const age = clampMaxActionFrameAge(maximumAge);
        auto const expiresAt = frame.capturedAt().checkedAdd(age);
        if (!expiresAt)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format(
                    "observation deadline overflows the monotonic clock: capture {} plus age {}",
                    frame.capturedAt().timePoint().time_since_epoch().count(),
                    age.count()
                )
            );
        }

        return ObservationLease{
            frame.sessionId(),
            frame.targetGeneration(),
            frame.id(),
            *expiresAt
        };
    }

    auto ObservationLease::forRecordedFrame(Frame const& frame) noexcept
        -> ObservationLease
    {
        return ObservationLease{
            frame.sessionId(),
            frame.targetGeneration(),
            frame.id(),
            std::nullopt
        };
    }

    auto ObservationLease::sessionId() const noexcept -> CaptureSessionId { return m_sessionId; }
    auto ObservationLease::targetGeneration() const noexcept -> TargetGeneration
    {
        return m_targetGeneration;
    }
    auto ObservationLease::frameId() const noexcept -> FrameId { return m_frameId; }
    auto ObservationLease::expiresAt() const noexcept -> std::optional<MonotonicInstant>
    {
        return m_expiresAt;
    }

    auto ObservationLease::isExpired(MonotonicInstant now) const noexcept -> bool
    {
        return m_expiresAt.has_value() && now > *m_expiresAt;
    }

    auto ObservationLease::validate(
        CaptureSessionId currentSession,
        TargetGeneration currentGeneration,
        FrameId observedFrame,
        MonotonicInstant now
    ) const -> Status
    {
        if (m_sessionId != currentSession)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                std::format(
                    "lease session {} != current {}",
                    m_sessionId.value(),
                    currentSession.value()
                )
            );
        }

        if (m_targetGeneration != currentGeneration)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                std::format(
                    "lease generation {} != current {}",
                    m_targetGeneration.value(),
                    currentGeneration.value()
                )
            );
        }

        if (m_frameId != observedFrame)
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                std::format(
                    "lease frame {} != observed {}",
                    m_frameId.value(),
                    observedFrame.value()
                )
            );
        }

        if (isExpired(now))
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "lease expired: observation older than max action frame age"
            );
        }

        return ok();
    }
}
