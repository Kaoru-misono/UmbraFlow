#pragma once

#include "catalog.hpp"
#include "recognition.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/detection.hpp>
#include <domain/time.hpp>

namespace uf::annotation
{
    class ActionDetection final
    {
        ProjectId m_projectId;
        RecognizerId m_recognizerId;
        Detection m_detection;

        ActionDetection(
            ProjectId projectId,
            RecognizerId recognizerId,
            Detection detection
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            RecognitionCatalog const& catalog,
            RecognizerId recognizerId,
            Detection detection
        ) -> Result<ActionDetection>;

        [[nodiscard]]
        auto projectId() const noexcept UF_LIFETIME_BOUND -> ProjectId const&;

        [[nodiscard]] auto recognizerId() const -> RecognizerId;

        [[nodiscard]]
        auto detection() const noexcept UF_LIFETIME_BOUND -> Detection const&;
    };

    struct ActionDeliveryState final
    {
        ProjectFingerprint m_liveFingerprint;
        SessionId m_sessionId;
        TargetGeneration m_targetGeneration;
        FrameId m_frameId;
        MonotonicInstant m_now;
    };

    [[nodiscard]]
    auto authorizeCoordinateAction(
        RecognitionCatalog const& catalog,
        ResolvedPage const& resolvedPage,
        ActionDetection const& actionDetection,
        ObservationLease const& lease,
        ActionDeliveryState delivery
    ) -> Status;
}
