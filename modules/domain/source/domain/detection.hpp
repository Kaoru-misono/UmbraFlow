#pragma once

#include "frame.hpp"

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <optional>

namespace uf
{
    inline constexpr auto k_defaultMaxActionFrameAge = std::chrono::duration_cast<MonotonicInstant::Duration>(
        std::chrono::milliseconds{750}
    );

    [[nodiscard]]
    auto clampMaxActionFrameAge(
        MonotonicInstant::Duration requested
    ) noexcept -> MonotonicInstant::Duration;

    // Whether the target one capture came from advances on its own.
    //
    // It decides what the interval since a capture is evidence of. A live
    // target moves whether or not it is watched, so that interval bounds how
    // far the target may have drifted from the frame an action was measured on.
    // A source replaying fixed bytes produces the same pixels for as long as it
    // exists, so the same interval measures how long the observer took and says
    // nothing about the target.
    enum class TargetWorld : uint8
    {
        Live,
        Recorded,
    };

    class Detection final
    {
        CaptureSessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId          m_frameId;
        Label            m_label;
        Rect<FrameSpace> m_rect;
        float            m_confidence;

    public:
        Detection(
            CaptureSessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId,
            Label label,
            Rect<FrameSpace> rect,
            float confidence
        );

        auto operator==(Detection const&) const -> bool = default;

        [[nodiscard]] auto sessionId() const noexcept -> CaptureSessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;
        [[nodiscard]]
        auto label() const noexcept UF_LIFETIME_BOUND -> Label const&;
        [[nodiscard]] auto rect() const noexcept -> Rect<FrameSpace>;
        [[nodiscard]] auto confidence() const noexcept -> float;
    };

    class ObservationLease final
    {
        CaptureSessionId                m_sessionId;
        TargetGeneration                m_targetGeneration;
        FrameId                         m_frameId;
        std::optional<MonotonicInstant> m_expiresAt;

        constexpr ObservationLease(
            CaptureSessionId sessionId,
            TargetGeneration targetGeneration,
            FrameId frameId,
            std::optional<MonotonicInstant> expiresAt
        ) noexcept
            : m_sessionId{sessionId}
            , m_targetGeneration{targetGeneration}
            , m_frameId{frameId}
            , m_expiresAt{expiresAt}
        {
        }

    public:
        auto operator==(ObservationLease const&) const -> bool = default;

        // The lease one capture of a live target carries: usable until
        // `maximumAge` after the capture, shortened to k_defaultMaxActionFrameAge
        // when the caller asks for longer.
        [[nodiscard]]
        static auto forFrame(
            Frame const& frame,
            MonotonicInstant::Duration maximumAge
        ) -> Result<ObservationLease>;

        // The lease one capture of a recorded target carries. It has no
        // deadline, and that is not a weaker lease: a recording produces the
        // same pixels for as long as its source exists, so an elapsed interval
        // is evidence about the observer and never about the target. The
        // identity clauses -- capture session, target generation, frame -- are
        // unchanged, and they are what still refuses a lease presented against
        // another observation.
        //
        // Nothing that can reach a real window mints one: TargetWorld comes from
        // the frame source rather than from configuration, and
        // EngineSession::create refuses to pair a recorded source with a sink
        // that posts to a live target.
        [[nodiscard]]
        static auto forRecordedFrame(Frame const& frame) noexcept -> ObservationLease;

        [[nodiscard]] auto sessionId() const noexcept -> CaptureSessionId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto frameId() const noexcept -> FrameId;

        // Disengaged exactly when the lease is over a recorded target.
        [[nodiscard]]
        auto expiresAt() const noexcept -> std::optional<MonotonicInstant>;

        [[nodiscard]] auto isExpired(MonotonicInstant now) const noexcept -> bool;

        [[nodiscard]]
        auto validate(
            CaptureSessionId currentSession,
            TargetGeneration currentGeneration,
            FrameId observedFrame,
            MonotonicInstant now
        ) const -> Status;
    };
}
