#pragma once

#include "catalog.hpp"
#include "recognition.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/detection.hpp>
#include <domain/time.hpp>

namespace uf::annotation
{
    // Keep ownership-bearing constructor sinks reference-based. LLVM 23's
    // performance-unnecessary-value-param check can recurse through StrongValue
    // construction when those sinks are passed by value.
    class ActionDetection final
    {
        ProjectId    m_projectId;
        RecognizerId m_recognizerId;
        Detection    m_detection;

        ActionDetection(
            ProjectId&& projectId,
            RecognizerId recognizerId,
            Detection&& detection
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
        ProjectFingerprint liveFingerprint;
        CaptureSessionId   sessionId;
        TargetGeneration   targetGeneration{};
        FrameId            frameId;
        MonotonicInstant   now;
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
