#pragma once

#include "agent-profile.hpp"
#include "controller.hpp"
#include "effective-plan.hpp"
#include "journal-entry.hpp"
#include "manifest.hpp"
#include "operation.hpp"
#include "project-observation.hpp"
#include "project-plugin.hpp"
#include "reconcile-outcome.hpp"
#include "tool-invocation.hpp"

#include <task/host-delivery.hpp>
#include <task/runtime-model-file.hpp>
#include <task/ui-observation.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <core/types/strong-value.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>

#include <compare>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <variant>
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
        std::string controlledTargetId{};
        std::string projectInstanceKey{};
        SessionMode mode{SessionMode::Read};

        // Which of the three operators this session is. It is pinned with the
        // rest of the immutable session tuple rather than chosen per call, so a
        // controller cannot change what it is between two commands. Agent is
        // the default because it is the least privileged of the three.
        ControllerKind kind{ControllerKind::Agent};
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

    // The kinds of controller-visible fact appended to the ledger's one ordered
    // event sequence. Every value has a producer; a value nothing writes would
    // be a promise with no code, so the enumeration grows with its producer
    // rather than ahead of it, and the DDL's CHECK lists exactly these.
    enum class LedgerEventKind : uint8
    {
        OperationCreated,
        ControlTransitioned,
        ExternalInputDetected,
    };

    // How far a reader has got through that sequence: the sequence number of
    // the last event it has consumed, and 0 before the first one.
    //
    // The subscription IS this integer. The Operator holds nothing per
    // subscriber -- no callback, no registration, no per-reader row. A stored
    // callback would be a borrow with no backing owner, it would run controller
    // code inside the write transaction the fence and the mutation chain live
    // in, and it would put a second piece of authority beside the coordinator.
    struct SubscriptionCursor final
    {
        uint64 value{};

        auto operator<=>(SubscriptionCursor const&) const = default;
    };

    // One controller-visible fact. It names no receipt, coordinate, fencing
    // token, plan hash, tool name or canonical argument, so handing one to an
    // online Agent cannot widen the p03 ceiling.
    struct LedgerEvent final
    {
        SubscriptionCursor sequence{};
        LedgerEventKind    kind{LedgerEventKind::OperationCreated};
        std::string        controlledTargetId{};
        std::string        subjectId{};

        auto operator==(LedgerEvent const&) const -> bool = default;
    };

    struct SubscriptionBatch final
    {
        std::vector<LedgerEvent> events{};

        // What to pass as `after` next time: the sequence of the last event in
        // `events`, or the requested cursor unchanged when there was nothing to
        // deliver. It is deliberately not the head of the stream -- a batch
        // truncated by maximumEvents whose cursor named the head would silently
        // skip everything the truncation left behind.
        SubscriptionCursor nextCursor{};

        auto operator==(SubscriptionBatch const&) const -> bool = default;
    };

    // The read could not be served losslessly, so it is refused rather than
    // truncated: a reader handed a gap has no way to tell that it has one.
    //
    // oldestAvailableCursor is read from the table rather than assumed. Nothing
    // prunes ledger_events yet, so it is always 0 and the only cause a caller
    // can produce today is a cursor from ahead of the head -- one minted
    // against another database or another epoch. The branch that compares
    // against it arrives with the pruning pass that makes it reachable.
    struct ResyncRequired final
    {
        SubscriptionCursor requestedCursor{};
        SubscriptionCursor oldestAvailableCursor{};
        SubscriptionCursor currentCursor{};

        auto operator==(ResyncRequired const&) const -> bool = default;
    };

    using SubscriptionRead = std::variant<SubscriptionBatch, ResyncRequired>;

    struct ControlLease final
    {
        std::string leaseId{};
        std::string sessionId{};
        std::string controlledTargetId{};
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

        // The join point between a snapshot and the event stream: the head of
        // ledger_events read inside the same BEGIN IMMEDIATE that composed this
        // record, so nothing can have committed between the two. A controller
        // that subscribes from here sees every event caused after the world it
        // is looking at, exactly once and in order.
        //
        // It is deliberately not a snapshots column. Nothing reads it back --
        // the snapshot's own staleness is decided by the revisions and the
        // findings the token join already compares -- and a stored column no
        // reader consumes is a fact with nothing keeping it true.
        SubscriptionCursor eventCursor{};
    };

    // Everything about a command that is the caller's to say. The tool, its
    // version, its arguments, its mutability and its surface are not here: they
    // arrive as a ValidatedToolInvocation the Tool Catalog owner minted, so
    // that a caller cannot present a mutating tool as read-only and escape the
    // mutation chain, nor a privileged tool as semantic and escape the Agent
    // ceiling. session_id is not here either: it has exactly one spelling, on
    // the ControllerBinding.
    struct CommandRequest final
    {
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

    // What one accepted submission settled. The fingerprint is
    // sha256(tool \0 version \0 args), derived inside the submitting
    // transaction from the bytes the catalog owner recognised. It is returned
    // so that two submissions can be proved to be one command -- by whom is
    // deliberately not among the hashed bytes, which is what makes the shared
    // Operation path provable -- and it is not a field any caller may state.
    //
    // No in-class initializer for the hash: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct AcceptedCommand final
    {
        StoredOperation operation;
        ContentHash     commandFingerprint;
    };

    // What one frozen plan settled. Every member is derived inside freezePlan's
    // transaction from bytes the ledger already held, so a caller reads them
    // here and can no longer state them anywhere.
    //
    // No in-class initializer for the hashes: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
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
    //
    // frozenPlanHash and dispatchSequence are deliberately not repeated beside
    // authority: they live there and are read from there, because the value the
    // Host is handed and the value the ledger later matches its own rows
    // against must be one value and not two that agree today.
    //
    // No in-class initializer for the hashes: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct DispatchReservation final
    {
        task::DispatchAuthority authority;
        ContentHash             decisionBasisHash;
        ContentHash             stepIntentHash;
        uint64                  operationRevision{};
        uint64                  stepIndex{};
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

    // What one human takeover did: the lease the new controller now holds, and
    // the dispatches this takeover found unanswered and resolved to
    // transport_unknown. The count is returned rather than logged because
    // "nothing was in flight" and "one effect may already have landed" are
    // different situations for the caller.
    struct ControlTakeover final
    {
        ControlLease lease;
        uint64       resolvedDispatches{};
    };

    // The fence a Host must adopt to act under this lease. Derived, never
    // stored: one lease has exactly one fence, so a second copy of it in the
    // database would be a second thing to keep true.
    [[nodiscard]]
    auto controlFence(ControlLease const& lease) -> task::ControlFence;

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

    // What an external input requires of the automation that was mid-flight
    // when it happened. Both values freeze; they differ in what has to happen
    // before anything moves again.
    enum class ExternalInputAction : uint8
    {
        FreezeAndReobserve,
        FreezeAndReconcile,
    };

    // Out-of-band input is a finding, not a command: it reports that the world
    // moved under us. This type is deliberately unable to name a tool, a tool
    // version, arguments, a mutability, a surface, a snapshot token or a
    // request id, and no overload of recordExternalInput takes a
    // ValidatedToolInvocation. The parameter list is the guarantee -- there is
    // no shape here that a dispatcher could act on, so no caller can smuggle a
    // command through this door.
    //
    // reason is free text and lands in a column nothing resolves a tool from;
    // a caller who writes a tool name into it produces a row the Operator will
    // never dispatch, because dispatch reads operations and operation_steps and
    // this row is in neither.
    struct ExternalInputReport final
    {
        ExternalInputAction requiredAction{ExternalInputAction::FreezeAndReobserve};
        std::string         reason{};
    };

    // What one recorded finding settled. Every member is derived inside the
    // recording transaction: the cursor and the invalidated revision are read
    // from the ledger, and operationId names whichever Operation the finding
    // froze, or nothing when the target had none in flight.
    struct RecordedExternalInput final
    {
        std::string                findingId{};
        uint64                     detectedAfterCursor{};
        uint64                     invalidatedSnapshotRevision{};
        std::optional<std::string> operationId{};
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

        // The trusted setup door, and the only place an Agent's ceilings are
        // established. agentProfile is required for exactly the kinds whose
        // ControllerProfile says budgetsRequired and refused for the others,
        // and it must be the profile this manifest pins -- so no path that
        // takes a ControllerBinding can state, raise or refresh a budget.
        [[nodiscard]]
        auto pinSession(
            SessionPin const& pin,
            SessionManifest const& manifest,
            std::optional<AgentProfile> const& agentProfile
        ) -> Status;

        // The one door onto the Operation path. Everything below takes a
        // ControllerBinding rather than a session id, so there is exactly one
        // spelling of "who is asking" and it is minted here from the pinned
        // sessions row. A binding is evidence and not a capability: every entry
        // point re-reads the row and refuses a binding whose epoch, kind or
        // activity has moved since it was minted.
        [[nodiscard]]
        auto bindController(
            std::string const& sessionId
        ) -> Result<ControllerBinding>;

        [[nodiscard]]
        auto acquireLease(
            ControllerBinding const& controller
        ) -> Result<ControlLease>;

        // Seizing control also closes what the displaced controller left in
        // flight: every dispatch nobody has answered for is resolved to
        // transport_unknown in the same transaction that bumps the fence, and
        // its Operation moves to reconciling. Never not_delivered -- a dispatch
        // the Host may already have posted is exactly what the third value
        // exists for.
        [[nodiscard]]
        auto takeoverLease(
            ControllerBinding const& controller,
            std::string const& reason
        ) -> Result<ControlTakeover>;

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

        // The one function that creates an Operation, and it cannot be called
        // without a ControllerBinding. Script, Agent and Human reach it by the
        // identical route and share one operations row, one command
        // fingerprint, one snapshot binding, one mutation-chain slot, one state
        // machine and one Journal; a kind varies nothing along it except the
        // tool surface it may present.
        //
        // The invocation must be minted by the Tool Catalog owner bound to the
        // same ProjectRegistration the session is pinned to; a mismatch is
        // refused rather than reconciled. Its surface is re-evaluated here
        // against the binding, because a controller can present an invocation
        // it was never offered.
        [[nodiscard]]
        auto submitCommand(
            ControllerBinding const& controller,
            CommandRequest const& request,
            ValidatedToolInvocation const& invocation
        ) -> Result<AcceptedCommand>;

        // Records that the world moved under us, which is what out-of-band
        // human input is. It is not an Operation and cannot become one: it
        // fabricates no authority, it is not a request anything could deny, and
        // it takes no invocation. Only a binding whose profile admits it may
        // report one.
        //
        // Its effect is to freeze whatever this target had in flight and to
        // invalidate every snapshot taken up to that point, so the next command
        // has to look again before it acts.
        [[nodiscard]]
        auto recordExternalInput(
            ControllerBinding const& reporter,
            ExternalInputReport const& report
        ) -> Result<RecordedExternalInput>;

        // Reads forward from a cursor over everything that happened to this
        // binding's controlled target, including what other controllers caused:
        // a controller that could see only its own events could not notice the
        // human takeover it most needs to notice.
        //
        // It is named subscribe because that is the requirement's word for the
        // cursor protocol. It registers nothing, blocks on nothing and stores
        // nothing, and it charges no budget: it reads facts the ledger already
        // recorded, mints nothing and moves nothing, so a ceiling on it would
        // be a ceiling on reading the audit trail.
        [[nodiscard]]
        auto subscribe(
            ControllerBinding const& controller,
            SubscriptionCursor after,
            uint32 maximumEvents
        ) -> Result<SubscriptionRead>;

        // What this binding has left. It is the one reader of the stored
        // counters, so a case that asserts a decrement happened is reading the
        // database rather than a number the same call computed. A binding whose
        // kind carries no budget has nothing to report and is refused.
        [[nodiscard]]
        auto remainingBudget(
            ControllerBinding const& controller
        ) -> Result<AgentBudgetRemaining>;

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

        // The single mint of dispatch authority: the returned
        // task::DispatchAuthority is the only one a Host will act on, because
        // every field of it is matched against these rows again when the report
        // comes back.
        //
        // runtimeGeneration is the one member the ledger cannot know. A Host
        // generation is a per-process counter the Host itself mints, and
        // sessions.installed_generation is a different quantity -- the CAS
        // generation of the installed artifact -- so echoing that column here
        // would make one name mean two things. It is therefore the caller's,
        // and it is safe as the caller's for the reason the whole authority is
        // plain data: naming the wrong generation can only make the Host refuse.
        [[nodiscard]]
        auto reserveDispatch(
            std::string const& operationId,
            uint64 expectedRevision,
            ControlLease const& lease,
            GenerationId runtimeGeneration,
            AuthorityDecisionId const& authorityDecisionId,
            std::optional<ApprovalGrant> const& approval
        ) -> Result<DispatchReservation>;

        // The Operation and the dispatch are read out of the report, because
        // the only dispatch this call may answer for is the one the Host was
        // authorized to perform, and a HostDeliveryReport is constructible only
        // by TaskHost. expectedRevision stays a parameter: it is the caller's
        // own read of the ledger, not the Host's.
        [[nodiscard]]
        auto recordDeliveryOutcome(
            ControlLease const& lease,
            uint64 expectedRevision,
            task::HostDeliveryReport const& report
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
