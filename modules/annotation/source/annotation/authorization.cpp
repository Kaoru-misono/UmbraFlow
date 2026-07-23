#include "authorization.hpp"

#include <domain/error.hpp>

#include <ranges>
#include <utility>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        auto identityOf(Detection const& detection) noexcept -> FrameIdentity
        {
            return FrameIdentity{
                detection.sessionId(),
                detection.targetGeneration(),
                detection.frameId()
            };
        }
    }

    ActionDetection::ActionDetection(
        ProjectId projectId,
        RecognizerId recognizerId,
        Detection detection
    ) noexcept
        : m_projectId{std::move(projectId)}
        , m_recognizerId{std::move(recognizerId)}
        , m_detection{std::move(detection)}
    {
    }

    auto ActionDetection::create(
        RecognitionCatalog const& catalog,
        RecognizerId recognizerId,
        Detection detection
    ) -> Result<ActionDetection>
    {
        auto const* p_recognizer = catalog.findRecognizer(recognizerId);
        if (
            p_recognizer == nullptr
            || p_recognizer->annotationType() != AnnotationType::ActionTarget
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "detection is not bound to a catalog action_target"
            );
        }

        if (p_recognizer->name().value() != detection.label().value())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "detection label does not match its bound recognizer identity"
            );
        }

        return ActionDetection{
            catalog.projectId(),
            recognizerId,
            std::move(detection)
        };
    }

    auto ActionDetection::projectId() const noexcept -> ProjectId const&
    {
        return m_projectId;
    }
    auto ActionDetection::recognizerId() const -> RecognizerId { return m_recognizerId; }
    auto ActionDetection::detection() const noexcept -> Detection const& { return m_detection; }

    auto authorizeCoordinateAction(
        RecognitionCatalog const& catalog,
        ResolvedPage const& resolvedPage,
        ActionDetection const& actionDetection,
        ObservationLease const& lease,
        ActionDeliveryState delivery
    ) -> Status
    {
        if (delivery.m_liveFingerprint != catalog.fingerprint())
        {
            return fail(
                AutomationErrorKind::TargetCompatibilityUnverified,
                "live size or DPI does not match the annotation project fingerprint"
            );
        }

        auto const& pageEvidence = resolvedPage.evidence();
        if (
            pageEvidence.projectId() != catalog.projectId()
            || actionDetection.projectId() != catalog.projectId()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "page and detection evidence must belong to the active annotation project"
            );
        }

        auto const* p_page = catalog.findPage(resolvedPage.pageId());
        auto const* p_recognizer = catalog.findRecognizer(actionDetection.recognizerId());
        if (
            p_page == nullptr
            || p_recognizer == nullptr
            || p_recognizer->annotationType() != AnnotationType::ActionTarget
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "page or action recognizer is absent from the active catalog"
            );
        }

        auto const allowedPageIds = p_recognizer->allowedPageIds();
        if (std::ranges::find(allowedPageIds, resolvedPage.pageId()) == allowedPageIds.end())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "action recognizer does not authorize the resolved page"
            );
        }

        auto const resolvedIdentity = pageEvidence.frameIdentity();
        auto const detectionIdentity = identityOf(actionDetection.detection());
        auto const deliveryIdentity = FrameIdentity{
            delivery.m_sessionId,
            delivery.m_targetGeneration,
            delivery.m_frameId
        };
        if (
            resolvedIdentity != detectionIdentity
            || resolvedIdentity != deliveryIdentity
        )
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "resolved page, detection, and delivery must use the same frame identity"
            );
        }

        return lease.validate(
            delivery.m_sessionId,
            delivery.m_targetGeneration,
            delivery.m_frameId,
            delivery.m_now
        );
    }
}
