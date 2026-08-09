#include "reconcile-outcome.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    ValidatedReconcileOutcome::ValidatedReconcileOutcome(
        ContentHash projectRegistrationHash,
        ContentHash reconcileSchemaManifestHash,
        std::string operationId,
        ValidatedDocument proposal,
        ReconcileDisposition disposition
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_reconcileSchemaManifestHash{reconcileSchemaManifestHash}
        , m_operationId{std::move(operationId)}
        , m_proposal{std::move(proposal)}
        , m_disposition{disposition}
    {
    }

    auto ValidatedReconcileOutcome::projectRegistrationHash() const
        -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto ValidatedReconcileOutcome::reconcileSchemaManifestHash() const
        -> ContentHash
    {
        return m_reconcileSchemaManifestHash;
    }

    auto ValidatedReconcileOutcome::operationId() const noexcept
        -> std::string const&
    {
        return m_operationId;
    }

    auto ValidatedReconcileOutcome::proposal() const noexcept
        -> ValidatedDocument const&
    {
        return m_proposal;
    }

    auto ValidatedReconcileOutcome::disposition() const noexcept
        -> ReconcileDisposition
    {
        return m_disposition;
    }

    ProjectReconcileSchemaOwner::ProjectReconcileSchemaOwner(
        ContentHash projectRegistrationHash,
        ContentHash reconcileSchemaManifestHash,
        ReconcileDispositionReader readDisposition
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_reconcileSchemaManifestHash{reconcileSchemaManifestHash}
        , m_readDisposition{std::move(readDisposition)}
    {
    }

    auto ProjectReconcileSchemaOwner::create(
        VerifiedProjectRegistration const& registration,
        std::string_view exactReconcileSchemaManifestBytes,
        ReconcileDispositionReader readDisposition
    ) -> Result<ProjectReconcileSchemaOwner>
    {
        if (!readDisposition)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "ProjectReconcileSchemaOwner requires a disposition reader"
            );
        }
        UF_TRY_VALUE(
            manifestHash,
            sha256(std::as_bytes(std::span{exactReconcileSchemaManifestBytes}))
        );
        if (manifestHash != registration.reconcilePayloadSchemaManifestHash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "reconcile payload schema manifest bytes do not match the "
                "registration's reconcile_payload_schema_manifest_hash"
            );
        }
        return ProjectReconcileSchemaOwner{
            registration.hash(),
            manifestHash,
            std::move(readDisposition),
        };
    }

    auto ProjectReconcileSchemaOwner::validate(
        std::string operationId,
        ValidatedDocument proposal
    ) const -> Result<ValidatedReconcileOutcome>
    {
        if (operationId.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "a reconcile outcome must name the Operation it concluded about"
            );
        }
        if (
            proposal.projectRegistrationHash() != m_projectRegistrationHash
            || proposal.function() != ProjectPluginFunction::Reconcile
            || proposal.direction() != ProjectDocumentDirection::Output
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "reconcile outcome requires this registration's reconcile output"
            );
        }
        UF_TRY_VALUE_CONTEXT(
            disposition,
            m_readDisposition(proposal.bytes()),
            "reading the disposition out of the reconcile output"
        );
        return ValidatedReconcileOutcome{
            m_projectRegistrationHash,
            m_reconcileSchemaManifestHash,
            std::move(operationId),
            std::move(proposal),
            disposition,
        };
    }
}
