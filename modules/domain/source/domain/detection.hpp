#pragma once

#include "frame.hpp"

#include <chrono>

namespace uf
{
    inline constexpr auto k_defaultMaxActionFrameAge = std::chrono::duration_cast<MonotonicInstant::Duration>(
        std::chrono::milliseconds{750}
    );

    [[nodiscard]]
    auto clampMaxActionFrameAge(
        MonotonicInstant::Duration requested
    ) noexcept -> MonotonicInstant::Duration;

    class Detection final
    {
        SessionId        m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;
        Label            m_label;
        Rect<FrameSpace> m_rect;
        float            m_confidence;

    public:
        Detection(
            SessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId,
            Label label,
            Rect<FrameSpace> rect,
            float confidence
        );

        auto operator==(Detection const&) const -> bool = default;

        [[nodiscard]] auto sessionId() const noexcept -> SessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
        [[nodiscard]] auto label() const -> Label;
        [[nodiscard]] auto rect() const noexcept -> Rect<FrameSpace>;
        [[nodiscard]] auto confidence() const noexcept -> float;
    };

    class ObservationLease final
    {
        SessionId        m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;
        MonotonicInstant m_expiresAt;

        constexpr ObservationLease(
            SessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId,
            MonotonicInstant expiresAt
        ) noexcept
            : m_sessionId{sessionId}
            , m_targetGeneration{targetGeneration}
            , m_frameId{frameId}
            , m_expiresAt{expiresAt}
        {
        }

    public:
        auto operator==(ObservationLease const&) const -> bool = default;

        [[nodiscard]]
        static auto forFrame(
            Frame const& frame,
            MonotonicInstant::Duration maximumAge
        ) -> Result<ObservationLease>;

        [[nodiscard]] auto sessionId() const noexcept -> SessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
        [[nodiscard]] auto expiresAt() const noexcept -> MonotonicInstant;

        [[nodiscard]] auto isExpired(MonotonicInstant now) const noexcept -> bool;

        [[nodiscard]]
        auto validate(
            SessionId currentSession,
            TargetGeneration currentGeneration,
            FrameId observedFrame,
            MonotonicInstant now
        ) const -> Status;
    };
}
