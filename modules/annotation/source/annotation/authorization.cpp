#include "authorization.hpp"

#include <domain/error.hpp>

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
        ProjectId&& projectId,
        ElementId recognizerId,
        Detection&& detection
    ) noexcept
        : m_projectId{std::move(projectId)}
        , m_recognizerId{recognizerId}
        , m_detection{std::move(detection)}
    {
    }

    auto ActionDetection::create(
        RecognitionCatalog const& catalog,
        ElementId recognizerId,
        Detection detection
    ) -> Result<ActionDetection>
    {
        auto const* p_recognizer = catalog.findRecognizer(recognizerId);
        if (
            p_recognizer == nullptr
            || !p_recognizer->capabilities().hasInteract()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "detection is not bound to an interactive catalog element"
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
            ProjectId{catalog.projectId()},
            recognizerId,
            std::move(detection)
        };
    }

    auto ActionDetection::projectId() const noexcept -> ProjectId const&
    {
        return m_projectId;
    }
    auto ActionDetection::recognizerId() const -> ElementId { return m_recognizerId; }
    auto ActionDetection::detection() const noexcept -> Detection const& { return m_detection; }

    auto authorizeCoordinateAction(
        RecognitionCatalog const& catalog,
        ResolvedPage const& resolvedPage,
        ActionDetection const& actionDetection,
        ObservationLease const& lease,
        ActionDeliveryState delivery
    ) -> Status
    {
        if (delivery.liveFingerprint != catalog.fingerprint())
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
            || !p_recognizer->capabilities().hasInteract()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "page or interactive element is absent from the active catalog"
            );
        }

        // Authorisation IS the reference. The separate allowed-page list said
        // exactly this and had to be kept equal to it by hand, with nothing
        // checking that it was.
        auto const* p_reference = catalog.findReference(
            resolvedPage.pageId(),
            actionDetection.recognizerId()
        );
        if (p_reference == nullptr || !p_reference->exercised.hasInteract())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "the resolved page does not exercise interact on this element"
            );
        }

        auto const resolvedIdentity = pageEvidence.frameIdentity();
        auto const detectionIdentity = identityOf(actionDetection.detection());
        auto const deliveryIdentity = FrameIdentity{
            delivery.sessionId,
            delivery.targetGeneration,
            delivery.frameId
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
            delivery.sessionId,
            delivery.targetGeneration,
            delivery.frameId,
            delivery.now
        );
    }
}
