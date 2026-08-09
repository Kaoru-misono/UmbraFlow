#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace uf::operator_runtime
{
    class ProjectReconcileSchemaOwner;

    // What a reconciliation concluded about the step that was dispatched. It is
    // the reconciler's conclusion, never the requester's: v1.7 lists these five
    // as the output of reconcile, and a caller able to relabel one could commit
    // a proposal that concluded Rejected as Confirmed.
    enum class ReconcileDisposition : uint8
    {
        Continue,
        Confirmed,
        Rejected,
        Ambiguous,
        Diverged,
    };

    // One reconcile output, together with the disposition read out of it by the
    // authority bound to the registration that pinned its schema. Only that
    // authority can mint one.
    class ValidatedReconcileOutcome final
    {
        friend class ProjectReconcileSchemaOwner;

        ContentHash          m_projectRegistrationHash;
        ContentHash          m_reconcileSchemaManifestHash;
        std::string          m_operationId;
        ValidatedDocument    m_proposal;
        ReconcileDisposition m_disposition;

        ValidatedReconcileOutcome(
            ContentHash projectRegistrationHash,
            ContentHash reconcileSchemaManifestHash,
            std::string operationId,
            ValidatedDocument proposal,
            ReconcileDisposition disposition
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto reconcileSchemaManifestHash() const -> ContentHash;

        // The Operation this conclusion is about. Without it an outcome is
        // only bound to a registration, so a Confirmed conclusion reached for
        // one Operation could be committed against another that happens to be
        // reconciling -- which is the relabel this authority exists to stop,
        // reached one step further out.
        [[nodiscard]]
        auto operationId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto proposal() const noexcept UF_LIFETIME_BOUND
            -> ValidatedDocument const&;

        [[nodiscard]] auto disposition() const noexcept -> ReconcileDisposition;
    };

    // Trusted deployment callback. It reads the disposition member out of a
    // reconcile output that the project's own schema has already accepted. It
    // is never passed to plugin code or published in a business VM.
    using ReconcileDispositionReader = std::function<
        Result<ReconcileDisposition>(std::string_view exactJcs)
    >;

    class ProjectReconcileSchemaOwner final
    {
        ContentHash                m_projectRegistrationHash;
        ContentHash                m_reconcileSchemaManifestHash;
        ReconcileDispositionReader m_readDisposition;

        ProjectReconcileSchemaOwner(
            ContentHash projectRegistrationHash,
            ContentHash reconcileSchemaManifestHash,
            ReconcileDispositionReader readDisposition
        );

    public:
        // The exact reconcile payload schema manifest bytes are required so the
        // reader provably answers for the schema this registration named.
        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            std::string_view exactReconcileSchemaManifestBytes,
            ReconcileDispositionReader readDisposition
        ) -> Result<ProjectReconcileSchemaOwner>;

        [[nodiscard]]
        auto validate(
            std::string operationId,
            ValidatedDocument proposal
        ) const -> Result<ValidatedReconcileOutcome>;
    };
}
