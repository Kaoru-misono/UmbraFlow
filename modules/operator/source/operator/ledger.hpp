#pragma once

#include "effective-plan.hpp"
#include "journal-entry.hpp"
#include "manifest.hpp"
#include "operation.hpp"
#include "project-observation.hpp"
#include "project-plugin.hpp"
#include "reconcile-outcome.hpp"
#include "tool-invocation.hpp"

#include <task/page-model-file.hpp>
#include <task/ui-observation.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

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

    // What one reclamation pass removed from the production RuntimeArtifact
    // root: content-addressed object directories no installation still names,
    // and staging directories no in-flight publication still names.
    struct ReclaimedRuntimeArtifacts final
    {
        uint64 artifactDirectories{};
        uint64 stagingDirectories{};
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

    // One published snapshot head. Every hash on it is derived inside the
    // publishing transaction from what that transaction read, so nothing here
    // is a value a caller could have named: identityHash answers "is this the
    // same composed world", decisionBasisHash answers "is this the same
    // decision input", and the two differ because a lease takeover moves the
    // first and must not move the second.
    struct SnapshotRecord final
    {
        std::string        token{};
        std::string        sessionId{};
        ContentHash        identityHash;
        ContentHash        decisionBasisHash;
        ContentHash        stateResolutionHash;
        ContentHash        projectStateHash;
        std::string        canonicalParts{};
        uint64             sessionEpoch{};
        uint64             leaseRevision{};
        uint64             snapshotRevision{};
        uint64             projectStateRevision{};
        uint64             availabilityRevision{};
        ProjectObservation observation;
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

    // What one frozen plan settled. Every member is derived inside freezePlan's
    // transaction from bytes the ledger already held, so a caller reads them
    // here and can no longer state them anywhere.
    struct FrozenPlan final
    {
        StoredOperation operation;
        ContentHash     planHash;
        ContentHash     decisionBasisHash;
        ContentHash     effectEnvelopeHash;
        WorkflowLimits  limits{};
        Risk            risk{Risk::Critical};
        bool            approvalRequired{};
    };

    // One minted workflow step. stepIntentHash covers the frozen plan hash and
    // the decimal index, so the same document at another index is another step.
    struct PlannedStep final
    {
        StoredOperation operation;
        ContentHash     stepIntentHash;
        std::string     stepKey{};
        uint64          stepIndex{};
        StepKind        kind{StepKind::Wait};
    };

    // What the dispatch was authorised against. The three hashes were caller
    // parameters until W2 and are now results: the Operation has at most one
    // pending UI-action step, so a dispatch names nothing and either finds that
    // step or fails.
    struct DispatchReservation final
    {
        ContentHash frozenPlanHash;
        ContentHash decisionBasisHash;
        ContentHash stepIntentHash;
        uint64      dispatchSequence{};
        uint64      operationRevision{};
        uint64      stepIndex{};
    };

    // The transitions a controller may ask for by name. The four plan-lifecycle
    // events are absent because the Operator decides them: a caller that could
    // say ReadyWithoutApproval could skip an approval the derived risk
    // required, and one that could say NextStepReady could advance a workflow
    // no plugin proposed a step for.
    enum class OperationSignal : uint8
    {
        ReadCompleted,
        DecisionInputsChanged,
        Revalidated,
        Invalidated,
        Denied,
        Cancelled,
        DeadlineExpired,
        NewEvidence,
        PostDispatchAbort,
    };

    enum class DeliveryOutcome : uint8
    {
        NotDelivered,
        Delivered,
        TransportUnknown,
    };

    // The identity of one authority decision. It is a strong type and not a
    // std::string because it travels beside operationId through reserveDispatch
    // and issueApproval: two interchangeable strings in an authorization path
    // swap silently at a call site, and neither the compiler nor a test that
    // asserts on the result can tell afterwards.
    struct AuthorityDecisionIdTag;
    using AuthorityDecisionId = StrongValue<AuthorityDecisionIdTag, std::string>;

    struct ApprovalGrant final
    {
        std::string         token{};
        AuthorityDecisionId authorityDecisionId;
    };

    // What a human approver states, and nothing else. The plan hash, the step
    // intent, the decision basis and the effect envelope are read from
    // operation_plans and the pending operation_steps row, because an approver
    // who could name them could issue an approval for a plan nobody froze.
    //
    // policyHash is still a caller field and is the one remaining hole: no code
    // parses a PolicyArtifact, so there is nothing to derive it from. It
    // belongs to c12 rather than to this change.
    struct ApprovalRequest final
    {
        std::string  operationId{};
        ControlLease lease;
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

        // Removes every production RuntimeArtifact directory the database no
        // longer references. It is explicit because a failed installation is
        // not proof that its directory is unwanted -- a concurrent publisher
        // may have put the identical bytes there -- so only a pass that reads
        // the whole reference set at once may decide.
        [[nodiscard]]
        auto reclaimUnreferencedRuntimeArtifacts() -> Result<ReclaimedRuntimeArtifacts>;

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

        // The Snapshot Coordinator. It reads every owner's revision under one
        // BEGIN IMMEDIATE, runs the project's derive against what it read, and
        // publishes one complete record before returning a token.
        //
        // There is no identity parameter and nothing replaces it: a caller that
        // supplied one could pin a snapshot to a world the ledger never held.
        // The two values it does take cannot be fabricated either -- a
        // ProjectPluginHandle comes only from ProjectPluginRegistrar::findExact,
        // and a UiObservationSnapshot only from TaskHost.
        [[nodiscard]]
        auto createSnapshot(
            ControlLease const& lease,
            ProjectPluginHandle const& plugin,
            task::UiObservationSnapshot const& observation
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
            OperationSignal signal
        ) -> Result<StoredOperation>;

        // The plan is minted here rather than by the caller because the command
        // fingerprint and the decision basis are the ledger's: they come from
        // the operations row and the snapshot row it names, and a plan that
        // stated its own would be a plan about a world nobody observed. The
        // Operation's own state moves to Ready or AwaitingApproval according to
        // the derived risk, so no caller chooses which.
        [[nodiscard]]
        auto freezePlan(
            std::string const& operationId,
            uint64 expectedRevision,
            ControlLease const& lease,
            ProjectPluginHandle const& plugin,
            OperatorPlanAuthority const& planAuthority
        ) -> Result<FrozenPlan>;

        // The next step of a frozen plan, at MAX(step_index) + 1 read inside the
        // same transaction that inserts it. It refuses past the frozen step
        // bound and while a UI-action step is still awaiting its dispatch.
        [[nodiscard]]
        auto mintNextStep(
            std::string const& operationId,
            uint64 expectedRevision,
            ControlLease const& lease,
            ProjectPluginHandle const& plugin,
            OperatorPlanAuthority const& planAuthority
        ) -> Result<PlannedStep>;

        [[nodiscard]]
        auto reserveDispatch(
            std::string const& operationId,
            uint64 expectedRevision,
            ControlLease const& lease,
            AuthorityDecisionId const& authorityDecisionId,
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
            AuthorityDecisionId const& authorityDecisionId
        ) -> Result<ApprovalGrant>;

        [[nodiscard]]
        auto commitReconciliation(
            ProjectPluginHandle const& plugin,
            ReconciliationCommit const& commit
        ) -> Result<StoredOperation>;
    };
}
