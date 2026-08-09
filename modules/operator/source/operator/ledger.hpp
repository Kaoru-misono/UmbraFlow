#pragma once

#include "journal-entry.hpp"
#include "manifest.hpp"
#include "operation.hpp"
#include "project-plugin.hpp"
#include "reconcile-outcome.hpp"
#include "tool-invocation.hpp"

#include <task/page-model-file.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uf::operator_runtime
{
    enum class SessionMode : uint8
    {
        Read,
        Write,
    };

    struct SessionPin final
    {
        std::string sessionId{};
        std::string authenticatedControllerId{};
        std::string idempotencyNamespace{};
        ContentHash projectRegistrationHash;
        ContentHash capabilityProfileHash;
        std::string controlledTargetKey{};
        std::string projectInstanceKey{};
        SessionMode mode{SessionMode::Read};
    };

    struct RuntimeArtifactInstallRequest final
    {
        std::filesystem::path handoffRoot;
        ContentHash           expectedReleaseManifestHash;
        uint64                expectedInstalledGeneration{};
    };

    struct ProjectInstanceBaseline final
    {
        std::string               projectInstanceKey{};
        std::string               eventId{};
        ContentHash               sessionManifestHash;
        ValidatedJournalEntryData entry;
    };

    struct ControlLease final
    {
        std::string leaseId{};
        std::string sessionId{};
        std::string controlledTargetKey{};
        std::string controllerId{};
        uint64      sessionEpoch{};
        uint64      fencingToken{};
        uint64      revision{};
        ContentHash capabilityProfileHash;
    };

    struct SnapshotRecord final
    {
        std::string token{};
        std::string sessionId{};
        ContentHash identityHash;
        uint64      sessionEpoch{};
        uint64      leaseRevision{};
    };

    // Everything about a command that is the caller's to say. The tool, its
    // version, its arguments and its mutability are not here: they arrive as a
    // ValidatedToolInvocation the Tool Catalog owner minted, so that a caller
    // cannot present a mutating tool as read-only and escape the mutation
    // chain.
    struct CommandRequest final
    {
        std::string sessionId{};
        std::string snapshotToken{};
        std::string idempotencyNamespace{};
        std::string clientRequestId{};
    };

    enum class CommandLookup : uint8
    {
        Created,
        Existing,
    };

    struct StoredOperation final
    {
        std::string    operationId{};
        CommandLookup  lookup{CommandLookup::Created};
        OperationState state{OperationState::Proposed};
        uint64         revision{};
        bool           planFrozen{};
        bool           hasDispatched{};
    };

    struct DispatchReservation final
    {
        uint64 dispatchSequence{};
        uint64 operationRevision{};
    };

    enum class DeliveryOutcome : uint8
    {
        NotDelivered,
        Delivered,
        TransportUnknown,
    };

    struct ApprovalGrant final
    {
        std::string token{};
        std::string authorityDecisionId{};
    };

    struct ApprovalRequest final
    {
        std::string  operationId{};
        ControlLease lease;
        ContentHash  frozenPlanHash;
        ContentHash  stepIntentHash;
        ContentHash  decisionBasisHash;
        ContentHash  effectEnvelopeHash;
        ContentHash  policyHash;
        std::string  approverPrincipal{};
        ContentHash  approverCapabilityHash;
        uint64       expiresAtUnixMillis{};
    };

    struct JournalAppend final
    {
        std::string               eventId{};
        ValidatedJournalEntryData entry;
    };

    struct ReconciliationCommit final
    {
        std::string operationId{};
        uint64      expectedOperationRevision{};
        uint64      expectedProjectStateRevision{};

        // The reconcile output and the disposition read out of it by the
        // authority bound to this registration. They travel together because
        // they are one conclusion: a separate caller-set disposition would let
        // a proposal that concluded Rejected be committed as Confirmed.
        ValidatedReconcileOutcome outcome;

        // The events this reconciliation appends to the Project Journal. There
        // is deliberately no reducer input beside them: the Operator derives
        // the reducer's envelope from these entries and the ProjectState the
        // database currently holds, so that a caller cannot record event A and
        // materialize a state reduced from event B.
        std::vector<JournalAppend> journalEvents{};
    };

    // Trusted in-process control plane. This object is never installed in a
    // business VM or exposed through the project-plugin data boundary.
    class OperatorCoordinator final
    {
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        explicit OperatorCoordinator(std::unique_ptr<Impl> implementation);

        [[nodiscard]]
        auto recoverUncertainDispatches() -> Result<uint64>;

    public:
        OperatorCoordinator(OperatorCoordinator&&) noexcept;
        auto operator=(OperatorCoordinator&&) noexcept -> OperatorCoordinator&;
        OperatorCoordinator(OperatorCoordinator const&) = delete;
        auto operator=(OperatorCoordinator const&) -> OperatorCoordinator& = delete;
        ~OperatorCoordinator();

        [[nodiscard]]
        static auto open(
            std::filesystem::path const& runtimeDirectory
        ) -> Result<OperatorCoordinator>;

        [[nodiscard]] auto databasePath() const -> std::filesystem::path;

        [[nodiscard]]
        auto installRuntimeArtifact(
            RuntimeArtifactInstallRequest const& request
        ) -> Result<task::InstalledRuntimeArtifact>;

        [[nodiscard]]
        auto openInstalledRuntimeArtifact(
            uint64 installedGeneration,
            ContentHash const& artifactRootHash
        ) -> Result<task::InstalledRuntimeArtifact>;

        [[nodiscard]]
        auto registerProject(VerifiedProjectRegistration const& registration) -> Status;

        [[nodiscard]]
        auto provisionProjectInstance(
            VerifiedProjectRegistration const& registration,
            ProjectPluginHandle const& plugin,
            ProjectInstanceBaseline const& baseline
        ) -> Status;

        [[nodiscard]]
        auto pinSession(
            SessionPin const& pin,
            SessionManifest const& manifest
        ) -> Status;

        [[nodiscard]]
        auto acquireLease(
            std::string const& sessionId
        ) -> Result<ControlLease>;

        [[nodiscard]]
        auto takeoverLease(
            std::string const& sessionId,
            std::string const& reason
        ) -> Result<ControlLease>;

        [[nodiscard]]
        auto releaseLease(
            ControlLease const& lease
        ) -> Result<uint64>;

        [[nodiscard]]
        auto createSnapshot(
            ControlLease const& lease,
            ContentHash const& identityHash
        ) -> Result<SnapshotRecord>;

        // The invocation must be minted by the Tool Catalog owner bound to the
        // same ProjectRegistration the session is pinned to; a mismatch is
        // refused rather than reconciled.
        [[nodiscard]]
        auto createOrLoadOperation(
            CommandRequest const& request,
            ValidatedToolInvocation const& invocation
        ) -> Result<StoredOperation>;

        [[nodiscard]]
        auto transitionOperation(
            std::string const& operationId,
            uint64 expectedRevision,
            OperationEvent event
        ) -> Result<StoredOperation>;

        [[nodiscard]]
        auto reserveDispatch(
            std::string const& operationId,
            uint64 expectedRevision,
            ControlLease const& lease,
            ContentHash const& decisionBasisHash,
            ContentHash const& frozenPlanHash,
            ContentHash const& stepIntentHash,
            std::string const& authorityDecisionId,
            std::optional<ApprovalGrant> const& approval
        ) -> Result<DispatchReservation>;

        [[nodiscard]]
        auto recordDeliveryOutcome(
            std::string const& operationId,
            uint64 dispatchSequence,
            uint64 expectedRevision,
            DeliveryOutcome outcome
        ) -> Result<StoredOperation>;

        [[nodiscard]]
        auto issueApproval(
            ApprovalRequest const& request,
            std::string const& authorityDecisionId
        ) -> Result<ApprovalGrant>;

        [[nodiscard]]
        auto commitReconciliation(
            ProjectPluginHandle const& plugin,
            ReconciliationCommit const& commit
        ) -> Result<StoredOperation>;
    };
}
