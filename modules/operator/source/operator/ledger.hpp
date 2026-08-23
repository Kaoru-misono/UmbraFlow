#pragma once

#include "agent-profile.hpp"
#include "controller.hpp"
#include "effective-plan.hpp"
#include "journal-entry.hpp"
#include "manifest.hpp"
#include "project-generation.hpp"
#include "project-observation.hpp"
#include "project-plugin.hpp"
#include "tool-invocation.hpp"
#include "tool-runtime.hpp"

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
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

        // What this controller may ask for, stated as the set itself rather
        // than as an opaque hash of one. capability_profile_hash is derived
        // from these names inside pinSession, so the two cannot disagree, and a
        // policy rule naming a required controller capability now has something
        // to be judged against.
        std::vector<std::string> controllerCapabilities{};

        std::string controlledTargetId{};
        std::string projectInstanceKey{};
        SessionMode mode{SessionMode::Read};

        // Which of the three operators this session is. It is pinned with the
        // rest of the controller tuple rather than chosen per call, so a
        // controller cannot change what it is between two commands. Agent is
        // the default because it is the least privileged of the three.
        ControllerKind kind{ControllerKind::Agent};

        // The observed-instance world this session's observations mint in. It
        // is pinned with the rest of the immutable tuple and stored under the
        // same three columns observed_instance_bindings carry, so every
        // observation this session produces is bound to one scope and a later
        // step can use only an ID that scope and fresh membership both admit.
        ObservedInstanceWorldScope worldScope;
    };

    // The externally known facts that identify a prior session without
    // exposing its Operator-private session or ProjectInstance names.
    struct SessionResume final
    {
        std::string    authenticatedControllerId{};
        std::string    controlledTargetId{};
        SessionMode    mode{SessionMode::Read};
        ControllerKind kind{ControllerKind::Agent};
    };

    struct RuntimeArtifactInstallRequest final
    {
        std::filesystem::path handoffRoot;
        ContentHash           expectedReleaseManifestHash;
        uint64                expectedInstalledGeneration{};
    };

    // No field carries a default because the record has no default
    // construction to give one meaning: ContentHash has no zero value, so the
    // implicit default constructor is deleted and every pin must name both the
    // generation and the root it pins. A default on the generation alone
    // advertised a construction that does not exist.
    struct RuntimeArtifactPin final
    {
        uint64      installedGeneration;
        ContentHash artifactRootHash;
    };

    struct ReleaseCapabilityApproval final
    {
        ContentHash              artifactRootHash;
        std::vector<std::string> controllerCapabilities{};
        ContentHash              evidenceHash;
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
        std::string projectInstanceKey{};
        std::string eventId{};
        ContentHash sessionManifestHash;

        // No entry is the one spelling of a project with no baseline: the
        // plugin reduces an empty Journal prefix against null, no Journal row
        // is fabricated, and eventId must be empty. Operator still owns the
        // resulting initial ProjectState and every later reduction.
        std::optional<ValidatedJournalEntryData> entry{};
    };

    // The kinds of controller-visible fact appended to the ledger's one ordered
    // event sequence. Every value has a producer; a value nothing writes would
    // be a promise with no code, so the enumeration grows with its producer
    // rather than ahead of it, and the DDL's CHECK lists exactly these.
    enum class LedgerEventKind : uint8
    {
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
    //
    // It carries no detail member. Both surviving kinds are complete in their
    // kind and subject identity, so a nullable one would be a column no
    // producer can fill.
    struct LedgerEvent final
    {
        SubscriptionCursor sequence{};
        LedgerEventKind    kind{LedgerEventKind::ControlTransitioned};
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
    // oldestAvailableCursor is read from the retained table rather than
    // assumed. A cursor below it has lost rows to bounded retention; a cursor
    // above currentCursor belongs to another database or epoch. Both refuse
    // with this value instead of returning a batch that would hide the gap.
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
        std::string              token{};
        std::string              sessionId{};
        ContentHash              identityHash;
        ContentHash              decisionBasisHash;
        ContentHash              stateResolutionHash;
        ContentHash              projectStateHash;
        std::string              canonicalParts{};
        uint64                   sessionEpoch{};
        uint64                   leaseRevision{};
        uint64                   snapshotRevision{};
        uint64                   projectStateRevision{};
        uint64                   availabilityRevision{};
        ContentHash              policyHash;
        std::vector<OfferedTool> availableTools{};
        StoredProjectObservation observation;

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

    enum class ToolIdentityLookup : uint8
    {
        Created,
        Existing,
    };

    // The outcome-independent identity rows used by the replacement Tool
    // Runtime. They deliberately carry no session, admission or provider
    // result: those become separate append-only records in later slices.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct StoredToolRootRequest final
    {
        ContentHash        rootIdentity;
        ToolIdentityLookup lookup{ToolIdentityLookup::Created};
    };

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct StoredToolCallPosition final
    {
        ContentHash        callIdentity;
        ToolIdentityLookup lookup{ToolIdentityLookup::Created};
    };

    // What one Tool-call-native input delivery was authorized against.
    //
    // It carries the authority and nothing else. The ledger already holds the
    // durable dispatching row the caller's ToolCallPositionIdentity names, so
    // there is nothing for the Host to carry back and no second revision to
    // compare. The outcome is recorded by completeToolCallDispatch against the
    // dispatch token the executor holds, which is the token that crossed the
    // boundary in the first place.
    struct ToolCallDispatchReservation final
    {
        task::DispatchAuthority authority;
    };

    // The one classification a Host delivery may be recorded under, and the
    // LEDGER'S rather than the provider's.
    //
    // task::DeliveryOutcome is the only value that can prove an external effect
    // absent, and a provider free to choose its own completion would be
    // choosing its own classification -- including the one that unlocks a
    // Rejected disposition in reconciliation. The three outcomes map exactly
    // one way. Delivered is an engine receipt for an input that reached the
    // sink, so it is confirmed. NotDelivered is the Host reporting that it
    // consumed the authorization and posted nothing, which is proven_absent and
    // the only outcome that proves absence. TransportUnknown is the Host
    // reporting that it reached the delivery path and cannot say whether input
    // arrived, which is possible.
    [[nodiscard]]
    auto toolCallCompletionFor(task::HostDeliveryReport const& report)
        -> Result<ToolCallCompletion>;

    // What one human takeover did: the lease the new controller now holds.
    struct ControlTakeover final
    {
        ControlLease lease;
    };

    // The fence a Host must adopt to act under this lease. Derived, never
    // stored: one lease has exactly one fence, so a second copy of it in the
    // database would be a second thing to keep true.
    [[nodiscard]]
    auto controlFence(ControlLease const& lease) -> task::ControlFence;

    // The identity of one authority decision. It is a strong type and not a
    // std::string because it travels beside other opaque identifiers through an
    // authorization path: two interchangeable strings there swap silently at a
    // call site, and neither the compiler nor a test that asserts on the result
    // can tell afterwards.
    struct AuthorityDecisionIdTag;
    using AuthorityDecisionId = StrongValue<AuthorityDecisionIdTag, std::string>;

    struct ToolApprovalGrant final
    {
        std::string         token{};
        AuthorityDecisionId authorityDecisionId;
    };

    struct ToolApprovalRequest final
    {
        std::string approverCapability{};
        uint64      expiresAtUnixMillis{};
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
    // a caller who writes a tool name into it produces a row nothing executes,
    // because no seam reads a tool out of a finding.
    struct ExternalInputReport final
    {
        ExternalInputAction requiredAction{ExternalInputAction::FreezeAndReobserve};
        std::string         reason{};
    };

    // What one recorded finding settled. Every member is derived inside the
    // recording transaction: the cursor and the invalidated revision are both
    // read from the ledger, never from the reporter.
    struct RecordedExternalInput final
    {
        std::string findingId{};
        uint64      detectedAfterCursor{};
        uint64      invalidatedSnapshotRevision{};
    };

    struct JournalAppend final
    {
        std::string               eventId{};
        ValidatedJournalEntryData entry;
    };

    // What one running Tool call proposes to commit to its ProjectInstance's
    // Journal, and the whole of what a proposer is allowed to state.
    //
    // Section 7 makes a proposal call-bound: it is not a Journal event and not
    // a durable Project fact until one final CAS publishes it, and until then
    // it hangs from the exact call and incarnation that made it. The
    // ProjectInstance, the revision the batch is proposed against and the
    // recorded identity of every referenced outcome are therefore NOT here --
    // the Operator reads all three from rows it already holds, for the reason
    // the reduce envelope is assembled rather than accepted.
    //
    // referencedCalls names the effects these facts interpret, by call
    // identity. It is a declaration and never a derivation: a proposal that let
    // the ledger work out which effects it depended on would be compared
    // against itself at publication, and section 7's "cannot commit while that
    // effect is possible" would have nothing independent to be about.
    struct JournalBatchProposal final
    {
        std::vector<JournalAppend> events{};
        std::vector<ContentHash>   referencedCalls{};
    };

    // One persisted call-bound proposal, as the ledger holds it.
    //
    // priorProjectStateRevision is the revision the batch was frozen against
    // and is read here rather than stated: it is what the publishing CAS
    // compares the live ProjectState row to, so a caller able to name it could
    // publish onto a revision its facts were never computed from.
    //
    // No in-class initializer for the identity: ContentHash has no default
    // state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct StoredJournalProposal final
    {
        ContentHash        proposalIdentity;
        ToolIdentityLookup lookup{ToolIdentityLookup::Created};
        uint64             priorProjectStateRevision{};
    };

    // What one published batch left behind: the revision the candidate state
    // was published at, and the Journal sequence the batch reached. Both are
    // the ledger's own, and both are reported because "the commit landed" and
    // "it landed here" are different facts.
    //
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct PublishedJournalBatch final
    {
        ContentHash proposalIdentity;
        ContentHash projectStateHash;
        uint64      revision{};
        uint64      lastJournalSequence{};
    };

    // What one refold of a ProjectInstance's baseline found: the ProjectState
    // the database stores for it, and the ProjectState its complete retained
    // Journal prefix folds to when that prefix is read back out of the
    // database and run through the registration's own reducer.
    //
    // Both sides are carried, hashes and bytes, because the answer to "did the
    // fold move" is the comparison and not a flag: a caller that was handed
    // only a verdict could not say what differed, and a stored verdict would
    // be a third value agreeing with the two it was derived from.
    //
    // No in-class initializer for the hashes: ContentHash has no default state.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct RefoldedProjectState final
    {
        std::string projectInstanceKey{};

        // How many Journal events the prefix held. It is reported because an
        // equality over an empty prefix is a weaker fact than one over a
        // prefix with events in it, and only the count says which happened.
        uint64 journalEventCount{};

        ContentHash storedStateHash;
        ContentHash refoldedStateHash;
        std::string storedCanonicalPayload{};
        std::string refoldedCanonicalPayload{};
    };

    // The registration, project instance and plugin facts one observed-instance
    // operation re-reads from the active lease. It is the Operator's own
    // vocabulary: no caller supplies one, and no proposal carries a field that
    // could stand in for it.
    struct ObservedInstanceContext final
    {
        std::string pluginId{};
        std::string pluginModuleManifestHash{};
        ContentHash projectRegistrationHash;
        std::string projectInstanceKey{};
    };

    // The one admission-request value, defined in tool-admission-request.hpp.
    // That header includes this one for ControlLease and ToolApprovalGrant, so
    // the edge runs one way and admitToolCall names the request by declaration.
    struct ToolAdmissionRequest;

    // The trusted idempotent provider query that resolves one uncertain
    // delivery, and the whole of the seam section 5.3's recovery table names.
    //
    // A crash that interrupts a mutating leaf leaves a durable row saying the
    // dispatch boundary was crossed and nothing saying whether the effect
    // landed. Only interrogating the world answers that, and only the
    // Coordinator may ask: it asks over the durable call position it holds,
    // after the row has been proven uncertain and the authority proven live, so
    // a querier cannot choose which call an answer is about. The answer's
    // evidence is mandatory and is stored beside the outcome it produced, which
    // is what makes the query durable rather than a judgement that evaporates.
    //
    // It is a query and not a provider: it performs no effect, it may be asked
    // again after a crash of its own, and no startup path can reach it -- which
    // is what keeps `proven_absent` from ever being inferred from a crash.
    using ToolReconciliationQuery = std::function<
        Result<ToolCallReconciliation>(ToolCallPositionIdentity const&)>;

    // Trusted in-process control plane. This object is never installed in a
    // business VM or exposed through the project-plugin data boundary.
    class OperatorCoordinator final
    {
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        explicit OperatorCoordinator(std::unique_ptr<Impl> implementation);

        [[nodiscard]]
        auto recoverUncertainDispatches() -> Result<uint64>;

        [[nodiscard]]
        auto recoverUncertainToolCalls() -> Result<uint64>;

        // The transaction-neutral canonical mint: the ten ordered checks
        // (shape, duplicate and parent-cycle relations, identity closure,
        // basis, stable mint, collision, consistency, parent projection,
        // canonicalization) that turn one proposal envelope into the final
        // closed observation. publishProjectObservation and createSnapshot
        // both call it inside their own transaction, so the production
        // observe path and the public API mint under the same rules.
        [[nodiscard]]
        auto mintProjectObservation(
            ObservedInstanceContext const& context,
            ObservedInstanceWorldScope const& worldScope,
            ObservedInstanceIdentitySchemas const& identitySchemas,
            ProjectIdentity const& project,
            ProjectObservationProposal const& proposal
        ) -> Result<ProjectObservation>;

        // Rebuilds the fresh observation a snapshot named from the exact bytes
        // the row stores. Only the bytes persist; this is the one parse the
        // Operator performs of a stored observation, and it happens so a step
        // can be held to the fresh membership of the observation it was minted
        // against.
        [[nodiscard]]
        auto restoreProjectObservation(std::string_view storedJcs)
            -> Result<ProjectObservation>;

    public:
        OperatorCoordinator(OperatorCoordinator&&) noexcept;
        auto operator=(OperatorCoordinator&&) noexcept -> OperatorCoordinator&;
        OperatorCoordinator(OperatorCoordinator const&) = delete;
        auto operator=(OperatorCoordinator const&) -> OperatorCoordinator& = delete;
        ~OperatorCoordinator();

        // Takes ownership of a runtime directory, and writes on the way in. It
        // creates the layout when absent, claims SQLite's exclusive lock for the
        // connection's lifetime, creates the schema on a first open, advances
        // runtime_state.current_session_epoch by one, deletes every
        // control_leases row, and clears sessions.active. A Tool call whose
        // durable dispatch boundary was crossed without an outcome becomes
        // possible/unknown, and is never handed out for dispatch again, when
        // and only when it is a mutating leaf; every other interrupted dispatch
        // survives as dispatching and is re-entered, because re-executing it
        // cannot deliver anything a durable row does not already classify.
        //
        // A non-empty schema reaches those restart writes only when its exact
        // stored-DDL identity is current or a migration is registered under
        // its exact source identity and the current exact target identity. The
        // migration verifies that target before its transaction commits and
        // records the pair it applied. An unregistered pair is refused without
        // changing the database.
        //
        // All of that is a restart, and it is what a coordinator must do before
        // it hands out a lease or dispatches an action: those rows are claims by
        // a process that is provably gone, since the exclusive lock is what let
        // this open happen at all. A caller that only wants to read an installed
        // generation is therefore asking for a restart it does not need --
        // readInstalledRuntimeArtifact below is the door for that.
        [[nodiscard]]
        static auto open(
            std::filesystem::path const& runtimeDirectory
        ) -> Result<OperatorCoordinator>;

        // Reads one installed generation's artifact pin and opens the artifact
        // it names, writing nothing at all.
        //
        // The connection carries SQLITE_OPEN_READONLY and not
        // SQLITE_OPEN_CREATE, so the guarantee is the library's rather than this
        // file's: no statement can write, and an absent database is refused
        // rather than created. Nothing on the path creates a directory either,
        // no exclusive ownership is claimed, no session epoch moves, no lease or
        // session is swept, and no dispatch is recovered. It requires the
        // current exact schema identity; even a registered source is refused
        // here because applying its migration would violate this door's
        // read-only contract.
        //
        // Skipping that sweep does not make the answer a lie. A
        // runtime_installations row and the runtime_state generation it advances
        // are written by one compare-and-swap transaction inside
        // installRuntimeArtifact, so a pin is never half-written and no recovery
        // stands between it and the truth. What the sweep resolves is control of
        // a target and effects that may already have landed on it, and this call
        // acquires no lease and dispatches nothing.
        //
        // It is static because there is no coordinator: constructing one is the
        // act this exists to avoid. While a coordinator does hold the directory,
        // its exclusive lock makes this call fail rather than read alongside it.
        [[nodiscard]]
        static auto readInstalledRuntimeArtifact(
            std::filesystem::path const& runtimeDirectory,
            uint64 installedGeneration,
            ContentHash const& artifactRootHash
        ) -> Result<task::InstalledRuntimeArtifact>;

        // Selects the active installation compatible with the caller-derived
        // artifact root and opens it without accepting the ledger's internal
        // compare-and-swap generation. This is a second read-only door, not a
        // Coordinator operation: it uses SQLITE_OPEN_READONLY and cannot run a
        // restart sweep or mutate session state.
        [[nodiscard]]
        static auto readActiveInstalledRuntimeArtifact(
            std::filesystem::path const& runtimeDirectory,
            ContentHash const& compatibleArtifactRootHash
        ) -> Result<task::InstalledRuntimeArtifact>;

        // The writable Coordinator selects its own active compatible release.
        // The installed generation is Operator-private CAS state, so a product
        // caller supplies only the artifact root derived from the project it
        // loaded. This is the sole writable-owner spelling; the static method
        // above remains the no-Coordinator read-only door.
        [[nodiscard]]
        auto openActiveInstalledRuntimeArtifact(
            ContentHash const& compatibleArtifactRootHash
        ) -> Result<task::InstalledRuntimeArtifact>;

        [[nodiscard]] auto databasePath() const -> std::filesystem::path;

        [[nodiscard]]
        auto installRuntimeArtifact(
            RuntimeArtifactInstallRequest const& request
        ) -> Result<task::InstalledRuntimeArtifact>;

        [[nodiscard]]
        auto activeRuntimeArtifactPin() -> Result<RuntimeArtifactPin>;

        [[nodiscard]]
        auto approveReleaseCapabilities(
            ReleaseCapabilityApproval const& approval
        ) -> Status;

        // Publishes through installRuntimeArtifact, then pins through the one
        // session door. A pin refusal activates the predecessor artifact again
        // at a new monotonic generation and records the failed attempt; the
        // pin's own transaction has already left no partial session tuple.
        [[nodiscard]]
        auto upgradeRuntimeArtifactAndPinSession(
            RuntimeArtifactInstallRequest const& installation,
            SessionPin const& pin,
            SessionManifest const& manifest,
            std::optional<AgentProfile> const& agentProfile
        ) -> Status;

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

        // Both doors take the registration identity rather than a registration
        // document, and provisioning takes the fold rather than the loaded
        // plugin that happens to carry it. Nothing durable here is a fact about
        // which generation of the registration document a project was deployed
        // as, so nothing here names one; the flip that replaces that document
        // therefore leaves this pair untouched.
        [[nodiscard]]
        auto registerProject(ProjectIdentity const& project) -> Status;

        [[nodiscard]]
        auto provisionProjectInstance(
            ProjectIdentity const& project,
            ProjectBaselineReducer const& reducer,
            ProjectInstanceBaseline const& baseline
        ) -> Status;

        // Re-derives one ProjectInstance's baseline from the Journal prefix the
        // database still holds, and hands back that answer beside the baseline
        // the database stored, so a caller can compare the two.
        //
        // Every input comes from a row. The events are read in `sequence`
        // order and re-validated through the journal owner this registration
        // pinned, the envelope is assembled by the same private builder
        // provisioning uses, and the fold runs on the reducer of the
        // registration the instance row names. Nothing a caller supplies
        // reaches the computation, which is what makes a disagreement between
        // the two answers a fact about the stored bytes rather than about the
        // call.
        //
        // It deliberately returns both answers rather than a verdict. A method
        // that returned `bool equal` would be the only reader of its own
        // comparison, and a test asserting that bool could not tell a real
        // match from a comparison that stopped being made; with both hashes
        // and both payloads in hand, the assertion lives where it can be read.
        [[nodiscard]]
        auto refoldProjectState(
            ProjectIdentity const& project,
            ProjectJournalSchemaOwner const& journal,
            ProjectBaselineReducer const& reducer,
            std::string const& projectInstanceKey
        ) -> Result<RefoldedProjectState>;

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

        // Reactivates the unique most-recent prior session matching these
        // externally known facts and the active installation named by the
        // manifest. Budgeted Agent sessions are refused because their steady
        // deadline is process-local; the caller must remain read-only or pin a
        // new Agent session under newly verified ceilings.
        [[nodiscard]]
        auto resumeSession(
            SessionResume const& resume,
            SessionManifest const& manifest
        ) -> Result<ControllerBinding>;

        // The one door onto the Tool Runtime path. Everything below takes a
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

        // Seizing control bumps the fence in the same transaction that records
        // the takeover, which is what strands the displaced controller: its
        // lease keeps its value and loses its authority.
        [[nodiscard]]
        auto takeoverLease(
            ControllerBinding const& controller,
            std::string const& reason
        ) -> Result<ControlTakeover>;

        // Voluntary release advances the fence and removes the lease in one
        // transaction. The returned fence is the one no later holder can reuse.
        [[nodiscard]]
        auto releaseLease(
            ControlLease const& lease
        ) -> Result<uint64>;

        // The Snapshot Coordinator. It reads every owner's revision under one
        // BEGIN IMMEDIATE and publishes one complete record before returning a
        // token.
        //
        // It runs no project code. Under the two-closure generation the
        // reducer exports `reduce` and nothing else, and a Project's reading of
        // its own world is a bound Project Tool the actors call -- so the
        // observation this composes is the empty proposal, and a Project that
        // wants to propose observed instances publishes them through
        // publishProjectObservation from inside such a call.
        //
        // There is no identity parameter beyond the registration's own and
        // nothing replaces it: a caller that supplied one could pin a snapshot
        // to a world the ledger never held. The values it does take cannot be
        // fabricated either -- a ProjectIdentity comes only from a verified
        // registration document, a ProjectToolCatalogSchemaOwner only from the
        // registered catalog, ObservedInstanceIdentitySchemas only from the
        // deployment that loaded the pinned registration, and a
        // UiObservationSnapshot only from TaskHost.
        [[nodiscard]]
        auto createSnapshot(
            ControlLease const& lease,
            ProjectIdentity const& project,
            ProjectToolCatalogSchemaOwner const& catalog,
            ObservedInstanceIdentitySchemas const& identitySchemas,
            task::UiObservationSnapshot const& observation
        ) -> Result<SnapshotRecord>;

        // Converts the VM's proposal into the locked final observation. The
        // registration, project instance and plugin identity are re-read from
        // the active lease; proposal carries no field that can replace them,
        // and no final ID enters through this signature.
        [[nodiscard]]
        auto publishProjectObservation(
            ControlLease const& lease,
            ProjectIdentity const& project,
            ObservedInstanceWorldScope const& worldScope,
            ObservedInstanceIdentitySchemas const& identitySchemas,
            ProjectObservationProposal const& proposal
        ) -> Result<ProjectObservation>;

        // Resolves an opaque wire spelling without parsing it. Authorization
        // is checked against the persistent binding before fresh-observation
        // membership, so a cross-scope ID cannot disclose whether it remains
        // visible in the scope that owns it.
        [[nodiscard]]
        auto resolveObservedInstance(
            ControlLease const& lease,
            ObservedInstanceWorldScope const& worldScope,
            ProjectObservation const& freshObservation,
            std::string_view observedInstanceId
        ) -> Result<ObservedInstanceId>;

        // Internal replacement-generation identity persistence. The root key
        // is unique only inside its authenticated caller namespace. Reusing it
        // with exact bytes rejoins; changing those bytes is a conflict.
        [[nodiscard]]
        auto persistToolRootRequest(
            ToolRootRequestIdentity const& root
        ) -> Result<StoredToolRootRequest>;

        // A root/parent/sequence position may carry exactly one immutable call
        // fingerprint. Exact replay rejoins it; changed caller-fixed material
        // is nondeterminism and cannot create another row at that position.
        [[nodiscard]]
        auto persistToolCallPosition(
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call
        ) -> Result<StoredToolCallPosition>;

        // The one admission function, and the only door to an admitted call.
        //
        // Per `caller independence is structural` every producer -- the Agent
        // adapter, the human-client adapter, the producer that admits an
        // actor's start at the top of a run, and one Tool calling another --
        // reaches authority by building one ToolAdmissionRequest and handing it
        // here. There is no second entry point and no shape that takes the
        // members loose, so an adapter cannot assemble its own path: what it can
        // construct is a request, and a request is not executable.
        //
        // Every authority member is re-read from the live binding, lease and
        // session; the request supplies only already-minted identity values,
        // what the call proposes, and what it presents. Read-only and mutating
        // share the identity, authority and budget path, and a mutating call
        // additionally enforces one active mutation per controlled target: a
        // possible or terminally-unresolved mutation is a durable target-wide
        // barrier across roots and actors.
        //
        // Which of the two a call is comes from the descriptor inside the
        // coordinate rather than from the producer, and the request's mutation
        // must agree with it. So must its delegation grant: a root-positioned
        // call carrying one and a child call missing one are both refused, and
        // there is no reading in which a child is admitted on the root's
        // authority.
        [[nodiscard]]
        auto admitToolCall(
            ToolAdmissionRequest const& request
        ) -> Result<ToolCallAdmission>;

        // The delegation grant one live handler invocation issues its children
        // on. It is minted only while the parent's durable state is
        // dispatching -- that is, only while the handler is actually running --
        // and only for a parent whose descriptor registered a child-effect
        // declaration. A Tool that declares no child effect can obtain no
        // grant, so removing that declaration is what makes a nested call fail
        // rather than merely narrowing it.
        [[nodiscard]]
        auto issueToolDelegationGrant(
            ToolCallPositionIdentity const& parentCall
        ) -> Result<ToolDelegationGrant>;

        // Closes one issuing context at teardown. Per R4 a restarted script
        // that terminates leaving recorded calls unconsumed for some context is
        // divergence, and this is where that is detected: the context knows how
        // many children it issued, and the ledger knows how many it recorded.
        //
        // The context is passed rather than a count so that the number cannot
        // be restated by the caller: the same object that assigned the indices
        // is the one that reports them.
        [[nodiscard]]
        auto sealToolCallContext(
            ToolRootRequestIdentity const& root,
            ToolCallIssuingContext const& context
        ) -> Status;

        // Mints one call-bound approval after re-evaluating the active
        // session's exact effect envelope and PolicyArtifact. The token is
        // consumed only by a matching admission attempt.
        [[nodiscard]]
        auto issueToolApproval(
            ControllerBinding const& controller,
            ControlLease const& lease,
            ControllerBinding const& approver,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            OperatorPolicyAuthority const& policyAuthority,
            std::span<ProposedEffect const> effects,
            ToolApprovalRequest const& request,
            AuthorityDecisionId const& authorityDecisionId
        ) -> Result<ToolApprovalGrant>;

        // Commits the dispatch boundary before provider code runs.
        [[nodiscard]]
        auto beginToolCallDispatch(
            ToolCallAdmission const& admission
        ) -> Result<ToolCallDispatch>;

        // Re-enters a call whose durable row is already dispatching, because
        // the incarnation that began that dispatch died inside it.
        //
        // The one shape refused is the MUTATING LEAF: a call answered directly
        // by a provider whose descriptor declares it mutating. That is the only
        // shape where the world may have moved with no durable row saying so,
        // and no amount of re-execution can find out, so its `dispatching` row
        // is resolved by classifying it uncertain and asking a reconciliation
        // query, never by dispatching it twice.
        //
        // Nothing else is refused, and neither half of that key is sufficient
        // alone. A call answered by a bound Project entry reaches the world
        // only through child Tool calls, each already carrying its own durable
        // classification, so re-running it re-derives the same child
        // coordinates, meets the recorded rows and executes only the first
        // position beyond history -- the replay contract itself, not a
        // redelivery, and true however mutating the call is. A read-only leaf
        // declares no effect for a delivery to be uncertain about, so running
        // its provider again delivers nothing twice.
        //
        // The presented binding and lease are the CURRENT ones and are re-read
        // live, which is what a fence bump is for: the incarnation that lost
        // the lease cannot re-enter, and the re-entry moves the history
        // revision so that a dispatch token the loser still holds no longer
        // matches the active dispatch. The origin principal and controlled
        // target must still be the ones the active admission attempt recorded;
        // re-entry re-enters an admission, it never widens one. It is stated
        // here rather than left to the authority join beginToolCallDispatch
        // uses, because that join pins the session epoch, lease and fence --
        // which is exactly what a re-entry after a takeover must be allowed to
        // move.
        [[nodiscard]]
        auto reenterToolCallDispatch(
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call
        ) -> Result<ToolCallDispatch>;

        // The one mint of Host delivery authority, over a Tool call that is
        // already dispatching.
        //
        // It names the call rather than carrying the ToolCallDispatch token,
        // and the reason is that the token proves less here than the row does.
        // The token restates an attempt number and a history revision the
        // ledger reads back anyway; what a delivery must know is that the
        // durable boundary is crossed RIGHT NOW, and only the row can say that.
        // A ToolCallPositionIdentity is constructible only inside the runtime,
        // so naming a call is not an authority a caller can invent, and a call
        // whose row is not `dispatching` is refused whoever presents it.
        //
        // uiTarget is the model target the call resolved. It is the caller's
        // because only the call knows which target its own resolved observation
        // named, and it is safe as the caller's for the reason the whole
        // authority is plain data: the Host refuses a Receipt whose intent
        // names another target, so naming the wrong one can only make the Host
        // refuse.
        //
        // runtimeGeneration is the one member the ledger cannot know. A Host
        // generation is a per-process counter the Host itself mints, and
        // sessions.installed_generation is a different quantity, so echoing
        // that column here would make one name mean two things. It is safe as
        // the caller's for the reason the whole authority is plain data: naming
        // the wrong generation can only make the Host refuse.
        [[nodiscard]]
        auto reserveToolCallDispatch(
            ToolCallPositionIdentity const& call,
            ControlLease const& lease,
            GenerationId runtimeGeneration,
            std::string const& uiTarget
        ) -> Result<ToolCallDispatchReservation>;

        // Records the exact provider conclusion. Repeating the same completion
        // rejoins; changing it after a terminal write is refused.
        [[nodiscard]]
        auto completeToolCallDispatch(
            ToolCallDispatch const& dispatch,
            ToolCallCompletion const& completion
        ) -> Result<StoredToolCallOutcome>;

        // Reads durable history only. It never consults a live lease and never
        // executes a provider, which is why terminal replay survives restart.
        [[nodiscard]]
        auto replayToolCall(
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call
        ) -> Result<ToolCallReplay>;

        // Reclassifies one possible mutating call by asking a trusted
        // idempotent provider query what the world says. Confirmed and
        // proven-absent release its target barrier; terminally-unresolved
        // remains a barrier by design.
        //
        // Only a `possible` call may be reconciled, and repeating a
        // reconciliation is refused rather than rejoined: the transition exists
        // to leave uncertainty, and once it is left there is nothing to
        // resolve. A caller that wants the outcome again reads it with
        // replayToolCall.
        [[nodiscard]]
        auto reconcileMutatingToolCall(
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            ToolReconciliationQuery const& query
        ) -> Result<ToolCallReplay>;

        // Persists one call-bound Journal batch proposal. Section 7: a
        // proposal is not a Journal event and not a durable Project fact at
        // this stage, so nothing here appends to the Journal or moves
        // ProjectState.
        //
        // The proposing call must be DISPATCHING, because a proposal is
        // something a running handler makes; the row records the revision that
        // call's outcome must carry, which is what binds the proposal to one
        // incarnation. A crashed incarnation's re-entry moves the history
        // revision, so the proposal it left behind names a revision the call
        // has moved past and the re-entered handler proposes its own.
        //
        // The identity is the content address of everything the proposer
        // stated, so re-proposing the same batch from the same incarnation
        // rejoins the stored row rather than freezing a second copy against a
        // later instant.
        [[nodiscard]]
        auto proposeJournalBatch(
            ControllerBinding const& controller,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            JournalBatchProposal const& proposal
        ) -> Result<StoredJournalProposal>;

        // The one final CAS of section 7. It re-verifies, against what the
        // proposal recorded, that the proposing call is still the same
        // confirmed incarnation, that every referenced effect still stands at
        // the outcome revision its facts were computed from, and that the
        // ProjectState is still at the revision the batch was frozen against;
        // it refuses while the run's controlled target is frozen, which is the
        // same statement as section 7's "cannot commit while that effect is
        // possible" because every uncertain call is a mutating call of this
        // run; it folds the frozen prior state and the prospective batch
        // through the registration's own reducer; and it appends the Journal
        // events and publishes the candidate state in one transaction. A crash
        // before it publishes neither fact nor state; a crash after it observes
        // both.
        //
        // The registration root is joined rather than stated: it is read from
        // the reducer this generation loaded and from the session row the
        // Operator pinned, which are two independently produced values, so a
        // reducer belonging to another registration is refused here.
        //
        // `journal` is required for the reason a refold requires one: the
        // frozen batch goes back through the schema owner this registration
        // pinned rather than being trusted as rows, so a publication cannot
        // append bytes the registration no longer admits.
        [[nodiscard]]
        auto publishJournalProposal(
            ControllerBinding const& controller,
            ControlLease const& lease,
            ProjectJournalSchemaOwner const& journal,
            ProjectBaselineReducer const& reducer,
            ContentHash const& proposalIdentity
        ) -> Result<PublishedJournalBatch>;

        // Records that the world moved under us, which is what out-of-band
        // human input is. It is not a Tool call and cannot become one: it
        // fabricates no authority, it is not a request anything could deny, and
        // it takes no invocation. Only a binding whose profile admits it may
        // report one.
        //
        // Its effect is to invalidate every snapshot taken up to that point, so
        // the next call has to look again before it acts.
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
    };

    // The durable half of the Tool Runtime protocol: the exact stored DDL TEXT
    // of every table a Tool call is recorded in, in a fixed order.
    //
    // It renders the DDL rather than a list of column names because a list is a
    // second spelling. The stored text carries the column set, its order, its
    // types, its nullability and every CHECK -- the durable state vocabulary
    // among them -- so a protocol change to any of those moves this material,
    // and no such change can move without it.
    //
    // It is deliberately NOT k_operatorDatabaseSchemaIdentity. That identity
    // covers every table this database has, sessions and snapshots included, so
    // an upgrade that touched only session storage would move it; these seven
    // tables are the ones a Tool run's record lives in, and they are the ones a
    // resuming incarnation must agree with its recorder about.
    [[nodiscard]]
    auto toolRuntimeDurableRecordMaterial() -> std::string;
}
