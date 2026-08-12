#include <operator/ledger.hpp>
#include <operator/operation.hpp>

#include "project-fixture.hpp"
#include "schema-binding.hpp"

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto repositoryRoot() -> std::filesystem::path
        {
            auto source = std::filesystem::path{__FILE__};
            if (source.is_relative())
            {
                source = std::filesystem::absolute(source);
            }
            auto candidate = source.parent_path().parent_path().parent_path();
            if (std::filesystem::is_directory(candidate / "schema"))
            {
                return candidate;
            }

            candidate = std::filesystem::current_path();
            while (!candidate.empty())
            {
                if (std::filesystem::is_directory(candidate / "schema"))
                {
                    return candidate;
                }
                auto const parent = candidate.parent_path();
                if (parent == candidate)
                {
                    break;
                }
                candidate = parent;
            }

            FAIL("repository root containing schema/ was not found");
            return {};
        }

        [[nodiscard]]
        auto readSchema(std::string_view filename) -> std::string
        {
            auto stream = std::ifstream{
                repositoryRoot() / "schema" / filename,
                std::ios::binary,
            };
            REQUIRE(stream.good());
            return {
                std::istreambuf_iterator<char>{stream},
                std::istreambuf_iterator<char>{},
            };
        }

        [[nodiscard]]
        auto definition(
            std::string const& schema,
            std::string_view name
        ) -> std::string
        {
            auto const declaration  = std::string{"\""} + std::string{name} + "\"";
            auto const namePosition = schema.find(declaration);
            REQUIRE(namePosition != std::string::npos);
            auto const begin = schema.find('{', namePosition + declaration.size());
            REQUIRE(begin != std::string::npos);

            auto depth    = std::size_t{};
            auto inString = false;
            auto escaped  = false;
            for (auto index = begin; index < schema.size(); ++index)
            {
                auto const character = schema[index];
                if (inString)
                {
                    if (escaped)
                    {
                        escaped = false;
                    }
                    else if (character == '\\')
                    {
                        escaped = true;
                    }
                    else if (character == '"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (character == '"')
                {
                    inString = true;
                }
                else if (character == '{')
                {
                    ++depth;
                }
                else if (character == '}')
                {
                    REQUIRE(depth > 0);
                    --depth;
                    if (depth == 0)
                    {
                        return schema.substr(begin, index - begin + 1);
                    }
                }
            }

            FAIL("schema definition has no closing object delimiter");
            return {};
        }

        auto checkStrictObject(std::string const& value) -> void
        {
            CHECK(value.find("\"type\": \"object\"") != std::string::npos);
            CHECK(value.find("\"additionalProperties\": false") != std::string::npos);
            CHECK(value.find("\"required\": [") != std::string::npos);
            CHECK(value.find("\"properties\": {") != std::string::npos);
        }

        using test_support::canonical;
        using test_support::hashOf;
        using test_support::journalEntry;
        using test_support::k_fixtureProvenance;
        using test_support::k_fixtureProvenanceViolations;
        using test_support::prepareStore;
        using test_support::reconciliationOutcome;
        using test_support::reconcilingOperation;
        using test_support::TemporaryDirectory;
    }

    // A human takeover and a Host delivery share one linearization, and the
    // acceptance text names the two things that buys. First: once a takeover
    // has returned, the fence it displaced cannot begin a dispatch -- the
    // ledger will not reserve one for it, and a Host still holding it will not
    // act on one that was reserved for someone else. Second: whatever was
    // already in flight is reported rather than guessed at. The takeover cannot
    // un-click what may already have landed; what it does is close the window
    // in which the ledger could still be told the effect did not happen.
    TEST_CASE("contract-agent-a07")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The Host the displaced controller keeps. It is built before the
        // takeover, so it carries the fence the takeover is about to supersede
        // and nothing ever tells it otherwise -- which is the state a displaced
        // controller is really in.
        auto staleHost = test_support::deliveringHost(prepared);

        auto const operation = test_support::createReadyOperation(
            prepared,
            "request-1",
            "command-1"
        );

        // A takeover with nothing in flight moves the fence and nothing else,
        // which is what leaves the clause below testing the lease alone.
        auto const takeoverBeforeDispatch = prepared.store.takeoverLease(
            prepared.controller,
            "a human takeover before anything was dispatched"
        );
        REQUIRE(takeoverBeforeDispatch.has_value());
        CHECK(takeoverBeforeDispatch->resolvedDispatches == 0U);

        auto const staleLease = prepared.lease;
        prepared.lease        = takeoverBeforeDispatch->lease;
        CHECK(staleLease.fencingToken < prepared.lease.fencingToken);

        auto host = test_support::deliveringHost(prepared);

        // A Host numbers its own generations, so these two name the same
        // GenerationId over the same installed artifact. That is what leaves
        // the fencing token as the single field separating the Host-side
        // refusal below from the delivery further down: without it the
        // authority would be foreign for a second reason and the refusal would
        // prove nothing about the fence.
        REQUIRE(staleHost->generation() == host->generation());

        // The first clause, ledger side. The Operation, its revision and the
        // Host generation are the three the reservation below succeeds with, so
        // the only thing this attempt is missing is a live lease.
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            staleLease,
            host->generation(),
            AuthorityDecisionId{"authority-displaced"},
            std::nullopt
        ).has_value());

        // Its positive control is the rest of this case: the same reservation
        // under the lease the takeover minted is what the in-flight half below
        // is built on, so the refusal above cannot be a takeover freezing the
        // target.
        auto const reserved = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(reserved.has_value());

        // The first clause, Host side. The displaced Host mints a Receipt of
        // its own and is handed a real authority, and still cannot begin this
        // dispatch: the fence it holds is the one the takeover replaced. Err
        // rather than a report, so nothing was consumed and nothing was posted.
        CHECK_FALSE(staleHost->deliver(reserved->authority).has_value());
        CHECK(staleHost->clicks() == 0U);

        // The Host acts. The click has landed and nothing in the ledger says so
        // yet: this is the whole of the race.
        auto const inFlight = host->deliverReport(reserved->authority);
        REQUIRE(inFlight.outcome() == task::DeliveryOutcome::Delivered);
        CHECK(host->clicks() == 1U);

        auto const takeover = prepared.store.takeoverLease(
            prepared.controller,
            "human takeover while a dispatch was in flight"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);

        // The takeover found the dispatch unanswered and resolved it in the
        // transaction that bumped the fence. The count is the difference
        // between "nothing was in flight" and "one effect may already have
        // landed", and the caller is told which.
        CHECK(takeover->resolvedDispatches == 1U);

        // The displaced controller still holds a real report. It is refused
        // twice over -- the lease it names is no longer the live row, and the
        // dispatch is no longer unanswered.
        CHECK_FALSE(prepared.store.recordDeliveryOutcome(
            prepared.lease,
            reserved->operationRevision,
            inFlight
        ).has_value());

        // What the ledger recorded instead is transport_unknown, which is not
        // proof of absence, so no reconciliation may conclude Rejected for this
        // Operation. Asserted through the one path that reads the column.
        auto const displacedLease = prepared.lease;
        prepared.lease            = takeover->lease;
        auto const resolved       = StoredOperation{
            .operationId = operation.operationId,
            .lookup      = CommandLookup::Existing,
            .state       = OperationState::Reconciling,
            .revision      = reserved->operationRevision + 1U,
            .planFrozen    = true,
            .hasDispatched = true,
        };
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = resolved.operationId,
                .expectedOperationRevision    = resolved.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    resolved.operationId,
                    "{\"disposition\":\"rejected\"}"
                ),
                .journalEvents                = {},
            }
        ).has_value());

        // The positive control for the revision above: the same Operation, the
        // same revision, a disposition transport_unknown does not forbid.
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = resolved.operationId,
                .expectedOperationRevision    = resolved.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    resolved.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());
        CHECK(displacedLease.fencingToken < prepared.lease.fencingToken);
    }

    // The reverse schedule. A recorded outcome is not re-opened by a takeover
    // that arrives after it, so "resolve what is unanswered" cannot become
    // "overwrite what was answered".
    TEST_CASE("a takeover after the outcome was recorded resolves nothing")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );

        auto const takeover = prepared.store.takeoverLease(
            prepared.controller,
            "human takeover after the outcome was recorded"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->resolvedDispatches == 0U);

        // The Operation is exactly where the recorded outcome left it: the
        // takeover neither advanced its revision nor moved its state.
        prepared.lease = takeover->lease;
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconciliationOutcome(
                    prepared,
                    operation.operationId,
                    "{\"disposition\":\"confirmed\"}"
                ),
                .journalEvents                = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(
                            prepared.project,
                            "fixture.confirmed",
                            "{\"value\":1}"
                        ),
                    },
                },
            }
        ).has_value());
    }

    TEST_CASE("schema-agent-a01")
    {
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const cursor = definition(schema, "SubscriptionCursor");
        auto const resync = definition(schema, "ResyncRequired");
        CHECK(cursor.find("\"type\": \"integer\"") != std::string::npos);
        CHECK(cursor.find("\"minimum\": 0") != std::string::npos);
        checkStrictObject(resync);
        CHECK(resync.find("\"requested_cursor\"") != std::string::npos);
        CHECK(resync.find("\"oldest_available_cursor\"") != std::string::npos);
        CHECK(resync.find("\"current_cursor\"") != std::string::npos);
    }

    TEST_CASE("schema-agent-a02")
    {
        auto const schema   = readSchema("umbraflow-operator-v1.schema.json");
        auto const budget   = definition(schema, "AgentBudget");
        auto const progress = definition(schema, "ProgressMarker");
        checkStrictObject(budget);
        checkStrictObject(progress);
        CHECK(budget.find("\"maximum_tool_calls\"") != std::string::npos);
        CHECK(budget.find("\"maximum_mutations\"") != std::string::npos);
        CHECK(budget.find("\"maximum_observations\"") != std::string::npos);
        CHECK(budget.find("\"maximum_risk_units\"") != std::string::npos);
        CHECK(progress.find("\"same_state_repetitions\"") != std::string::npos);
        CHECK(progress.find("\"elapsed_without_progress_ms\"") != std::string::npos);
    }

    TEST_CASE("contract-agent-a01")
    {
        auto const temporary = TemporaryDirectory{};
        auto       prepared  = prepareStore(temporary.path());

        // Pinned to the SAME controlled target the Script holds the lease on,
        // and holding no lease of its own. Watching is not controlling, and an
        // Agent that could see only what it caused could not notice the
        // takeover it most needs to notice.
        auto const agent = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-agent",
            "instance-agent",
            "target-1",
            test_support::k_unconstrainedAgentBudget
        );

        // The join point, read inside the transaction that published the
        // snapshot: nothing can have committed between the world it composed
        // and the stream position it names.
        auto const base = prepared.snapshot.eventCursor;

        auto const first = test_support::proposedOperation(
            prepared,
            "request-1",
            "observe-1"
        );
        auto const second = test_support::proposedOperation(
            prepared,
            "request-2",
            "raw-coordinate-click"
        );

        auto const human = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Write,
            "session-human",
            "instance-human",
            "target-1"
        );
        auto const takeover = prepared.store.takeoverLease(human, "a human took over");
        REQUIRE(takeover.has_value());

        // A whole controller's worth of activity on ANOTHER target, after all
        // of the above, so that the batch below can be shown to end where this
        // target's events end rather than where the stream does.
        auto const elsewhere = test_support::addController(
            prepared,
            ControllerKind::Script,
            SessionMode::Write,
            "session-elsewhere",
            "instance-elsewhere",
            "target-elsewhere"
        );
        auto const elsewhereLease = prepared.store.acquireLease(elsewhere);
        REQUIRE(elsewhereLease.has_value());
        auto const elsewhereSnapshot = prepared.store.createSnapshot(
            *elsewhereLease,
            prepared.plugin,
            test_support::observeAgain(prepared)
        );
        REQUIRE(elsewhereSnapshot.has_value());
        REQUIRE(prepared.store.submitCommand(
            elsewhere,
            test_support::command(*elsewhereSnapshot, "request-elsewhere"),
            test_support::toolInvocation(prepared.project, "observe-1")
        ).has_value());

        auto const read = prepared.store.subscribe(agent, base, 16U);
        REQUIRE(read.has_value());
        auto const* batch = std::get_if<SubscriptionBatch>(&*read);
        REQUIRE(batch != nullptr);

        // Exactly the three facts about this target, in order, starting at the
        // very next sequence after the snapshot's cursor. Two of them were
        // caused by a Script and one by a Human; none by the Agent reading them.
        REQUIRE(batch->events.size() == 3U);
        CHECK(batch->events[0].sequence.value == base.value + 1U);
        CHECK(batch->events[1].sequence.value == base.value + 2U);
        CHECK(batch->events[2].sequence.value == base.value + 3U);
        CHECK(batch->events[0].kind == LedgerEventKind::OperationCreated);
        CHECK(batch->events[1].kind == LedgerEventKind::OperationCreated);
        CHECK(batch->events[2].kind == LedgerEventKind::ControlTransitioned);
        CHECK(batch->events[0].subjectId == first.operationId);
        CHECK(batch->events[1].subjectId == second.operationId);
        CHECK(batch->events[2].subjectId == takeover->lease.leaseId);
        for (auto const& event : batch->events)
        {
            CHECK(event.controlledTargetId == "target-1");
        }
        CHECK(batch->nextCursor.value == base.value + 3U);

        // The elsewhere controller's own two events exist and are ahead of this
        // batch, which is what makes the scoping above an exclusion rather than
        // an accident of there being nothing else to see.
        auto const elsewhereRead = prepared.store.subscribe(elsewhere, base, 16U);
        REQUIRE(elsewhereRead.has_value());
        auto const* elsewhereBatch = std::get_if<SubscriptionBatch>(&*elsewhereRead);
        REQUIRE(elsewhereBatch != nullptr);
        REQUIRE(elsewhereBatch->events.size() == 2U);
        CHECK(elsewhereBatch->events[0].sequence.value == base.value + 4U);
        CHECK(elsewhereBatch->events[1].sequence.value == base.value + 5U);

        // A truncated batch resumes without a gap, because the cursor follows
        // what was delivered and not the head of the stream.
        auto const firstHalf = prepared.store.subscribe(agent, base, 2U);
        REQUIRE(firstHalf.has_value());
        auto const* firstBatch = std::get_if<SubscriptionBatch>(&*firstHalf);
        REQUIRE(firstBatch != nullptr);
        REQUIRE(firstBatch->events.size() == 2U);
        CHECK(firstBatch->nextCursor.value == base.value + 2U);
        auto const secondHalf = prepared.store.subscribe(
            agent,
            firstBatch->nextCursor,
            2U
        );
        REQUIRE(secondHalf.has_value());
        auto const* secondBatch = std::get_if<SubscriptionBatch>(&*secondHalf);
        REQUIRE(secondBatch != nullptr);
        REQUIRE(secondBatch->events.size() == 1U);
        CHECK(secondBatch->events[0].sequence.value == base.value + 3U);

        // The subscription is the cursor. Reading the same cursor twice returns
        // the same batch, because the Operator kept nothing from the first
        // read. This assertion has no one-line mutation that turns it red --
        // there is no line to break, only the absence of per-subscriber state
        // -- and it is kept as the guard that catches the day somebody adds
        // some.
        auto const again = prepared.store.subscribe(agent, base, 16U);
        REQUIRE(again.has_value());
        auto const* againBatch = std::get_if<SubscriptionBatch>(&*again);
        REQUIRE(againBatch != nullptr);
        CHECK(*againBatch == *batch);

        // Nothing left to read is an empty batch at the same cursor, not a
        // resync: the reader is level with the stream rather than off it.
        auto const drained = prepared.store.subscribe(agent, batch->nextCursor, 16U);
        REQUIRE(drained.has_value());
        auto const* drainedBatch = std::get_if<SubscriptionBatch>(&*drained);
        REQUIRE(drainedBatch != nullptr);
        CHECK(drainedBatch->events.empty());
        CHECK(drainedBatch->nextCursor == batch->nextCursor);

        // A cursor past the head is a cursor from another database or another
        // epoch, and is refused rather than answered with an empty batch.
        // oldest_available_cursor is read from the table: nothing prunes
        // ledger_events, so it is 0 while the head is five events further on.
        auto const ahead = prepared.store.subscribe(
            agent,
            SubscriptionCursor{base.value + 6U},
            16U
        );
        REQUIRE(ahead.has_value());
        auto const* resync = std::get_if<ResyncRequired>(&*ahead);
        REQUIRE(resync != nullptr);
        CHECK(resync->requestedCursor.value == base.value + 6U);
        CHECK(resync->currentCursor.value == base.value + 5U);
        CHECK(resync->oldestAvailableCursor.value == 0U);

        // A read of nothing is a caller error rather than an empty answer: it
        // cannot make progress and would loop for ever if it were served.
        CHECK_FALSE(prepared.store.subscribe(agent, base, 0U).has_value());
    }

    TEST_CASE("contract-agent-a02")
    {
        auto const temporary = TemporaryDirectory{};
        auto       prepared  = prepareStore(temporary.path());

        auto const pin = [&prepared](
            std::string sessionId,
            std::string projectInstanceKey,
            std::string controlledTargetId,
            ControllerKind kind
        )
        {
            return SessionPin{
                .sessionId                 = std::move(sessionId),
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = prepared.project.registration.hash(),
                .controllerCapabilities    = {std::string{conformance::k_operateCapability}},
                .controlledTargetId        = std::move(controlledTargetId),
                .projectInstanceKey        = std::move(projectInstanceKey),
                .mode                      = SessionMode::Write,
                .kind                      = kind,
            };
        };

        // A ProjectInstance nothing has pinned yet. The refusals below have to
        // fail for the reason under test, and an instance that already carries
        // an active write session refuses a second one on its own -- which is
        // exactly what masked all three of these when they named instance-1.
        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            prepared.plugin,
            ProjectInstanceBaseline{
                .projectInstanceKey  = "instance-pins",
                .eventId             = "baseline-pins",
                .sessionManifestHash = prepared.manifest.hash(),
                .entry               = journalEntry(
                    prepared.project,
                    prepared.project.registration.baselineEventType(),
                    "{\"kind\":\"baseline\"}"
                ),
            }
        ).has_value());

        // Which kinds carry ceilings is ControllerProfile's answer, and
        // pinSession holds both directions of it. An Agent without a profile
        // would be an Agent with no stopping condition; a Script with one would
        // be a budget nothing ever charges.
        auto const unconstrained = test_support::agentProfileFor(
            prepared,
            test_support::k_unconstrainedAgentBudget
        );
        CHECK_FALSE(prepared.store.pinSession(
            pin("session-unbudgeted", "instance-pins", "target-pins", ControllerKind::Agent),
            prepared.manifest,
            std::nullopt
        ).has_value());
        CHECK_FALSE(prepared.store.pinSession(
            pin("session-budgeted", "instance-pins", "target-pins", ControllerKind::Script),
            unconstrained.manifest,
            unconstrained.profile
        ).has_value());

        // The ceilings are the exact bytes the manifest attests to. A profile
        // verified against one manifest cannot be presented with another, and
        // bytes that do not hash to agent_profile_hash never become a profile
        // at all.
        CHECK_FALSE(prepared.store.pinSession(
            pin("session-crossed", "instance-pins", "target-pins", ControllerKind::Agent),
            prepared.manifest,
            unconstrained.profile
        ).has_value());

        // The positive control for the three refusals above: the same instance
        // and the same target, pinned with the profile this manifest attests
        // to. Without it, three refusals are consistent with an instance
        // nothing could be pinned to at all.
        REQUIRE(prepared.store.pinSession(
            pin("session-pins", "instance-pins", "target-pins", ControllerKind::Agent),
            unconstrained.manifest,
            unconstrained.profile
        ).has_value());

        CHECK_FALSE(AgentProfile::verifyExact(
            unconstrained.manifest,
            test_support::agentProfileBytes(AgentBudget{
                .maximumToolCalls     = 999U,
                .maximumMutations     = 999U,
                .maximumObservations  = 999U,
                .maximumElapsedMillis = 999U,
                .maximumRiskUnits     = 999U,
            }),
            test_support::agentProfileValidator()
        ).has_value());

        auto const leaseFor = [&prepared](ControllerBinding const& binding)
        {
            auto lease = prepared.store.acquireLease(binding);
            REQUIRE(lease.has_value());
            return *std::move(lease);
        };
        auto const snapshotFor = [&prepared](ControlLease const& lease)
        {
            return prepared.store.createSnapshot(
                lease,
                prepared.plugin,
                test_support::observeAgain(prepared)
            );
        };
        auto const submit = [&prepared](
            ControllerBinding const& binding,
            SnapshotRecord const& snapshot,
            std::string requestId,
            std::string_view toolName
        )
        {
            return prepared.store.submitCommand(
                binding,
                test_support::command(snapshot, std::move(requestId)),
                test_support::toolInvocation(prepared.project, std::string{toolName})
            );
        };

        // ACTION. One accepted command, and the second is refused by the
        // column's own CHECK rather than by a comparison beside it.
        auto const callsBudget = AgentBudget{
            .maximumToolCalls    = 1U,
            .maximumMutations    = 8U,
            .maximumObservations = 8U,
            .maximumElapsedMillis = 600'000U,
            .maximumRiskUnits     = 64U,
        };
        auto const calls = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-calls",
            "instance-calls",
            "target-calls",
            callsBudget
        );
        auto const callsLease    = leaseFor(calls);
        auto const callsSnapshot = snapshotFor(callsLease);
        REQUIRE(callsSnapshot.has_value());
        REQUIRE(submit(calls, *callsSnapshot, "request-1", "observe-1").has_value());
        auto const spent = submit(calls, *callsSnapshot, "request-2", "observe-1");
        REQUIRE_FALSE(spent.has_value());
        CHECK(automationErrorKind(spent.error()) == AutomationErrorKind::ActionRejected);
        auto const callsRemaining = prepared.store.remainingBudget(calls);
        REQUIRE(callsRemaining.has_value());
        CHECK(callsRemaining->toolCalls == 0U);
        CHECK(callsRemaining->observations == callsBudget.maximumObservations - 1U);

        // Exhaustion refuses new work; it does not un-record accepted work. The
        // replay of an accepted request still answers, and costs nothing,
        // because the counter records what the ledger accepted and this was
        // charged when it was accepted.
        auto const replay = submit(calls, *callsSnapshot, "request-1", "observe-1");
        REQUIRE(replay.has_value());
        CHECK(replay->operation.lookup == CommandLookup::Existing);

        // Pinning the same session again is idempotent and does NOT refresh
        // what it has spent. If it did, an exhausted Agent would only have to
        // ask for its own session twice.
        REQUIRE(prepared.store.pinSession(
            pin("session-calls", "instance-calls", "target-calls", ControllerKind::Agent),
            test_support::agentProfileFor(prepared, callsBudget).manifest,
            test_support::agentProfileFor(prepared, callsBudget).profile
        ).has_value());
        auto const afterRepin = prepared.store.remainingBudget(calls);
        REQUIRE(afterRepin.has_value());
        CHECK(afterRepin->toolCalls == 0U);

        // MUTATION. Sourced from the Tool Catalog descriptor and not from the
        // plan's declared risk, so a zero mutation ceiling refuses a mutating
        // tool while leaving every read-only one available.
        auto const mutations = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-mutations",
            "instance-mutations",
            "target-mutations",
            AgentBudget{
                .maximumToolCalls    = 8U,
                .maximumMutations    = 0U,
                .maximumObservations = 8U,
                .maximumElapsedMillis = 600'000U,
                .maximumRiskUnits     = 64U,
            }
        );
        auto const mutationsLease    = leaseFor(mutations);
        auto const mutationsSnapshot = snapshotFor(mutationsLease);
        REQUIRE(mutationsSnapshot.has_value());
        auto const refusedMutation = submit(
            mutations,
            *mutationsSnapshot,
            "request-1",
            "command-1"
        );
        REQUIRE_FALSE(refusedMutation.has_value());
        CHECK(
            automationErrorKind(refusedMutation.error())
            == AutomationErrorKind::ActionRejected
        );
        REQUIRE(
            submit(mutations, *mutationsSnapshot, "request-2", "observe-1").has_value()
        );

        // OBSERVATION. Charged by createSnapshot, in the same transaction and
        // before the plugin derive, so a refused budget never pays for one.
        auto const observations = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-observations",
            "instance-observations",
            "target-observations",
            AgentBudget{
                .maximumToolCalls    = 8U,
                .maximumMutations    = 8U,
                .maximumObservations = 1U,
                .maximumElapsedMillis = 600'000U,
                .maximumRiskUnits     = 64U,
            }
        );
        auto const observationsLease = leaseFor(observations);
        REQUIRE(snapshotFor(observationsLease).has_value());
        auto const refusedObservation = snapshotFor(observationsLease);
        REQUIRE_FALSE(refusedObservation.has_value());
        CHECK(
            automationErrorKind(refusedObservation.error())
            == AutomationErrorKind::ActionRejected
        );

        // RISK. The fixture's command-1 declares low and medium effects, so the
        // Operator derives medium, which the table prices at three units. Two
        // units is not enough and three is exactly enough, which is what pins
        // the table entry rather than merely the existence of a charge.
        auto const riskCase = [&](
            std::string const& suffix,
            uint64 riskCeiling
        )
        {
            auto const binding = test_support::addController(
                prepared,
                ControllerKind::Agent,
                SessionMode::Write,
                "session-risk-" + suffix,
                "instance-risk-" + suffix,
                "target-risk-" + suffix,
                AgentBudget{
                    .maximumToolCalls    = 8U,
                    .maximumMutations    = 8U,
                    .maximumObservations = 8U,
                    .maximumElapsedMillis = 600'000U,
                    .maximumRiskUnits     = riskCeiling,
                }
            );
            auto const lease    = leaseFor(binding);
            auto const snapshot = snapshotFor(lease);
            REQUIRE(snapshot.has_value());
            auto const operation = submit(binding, *snapshot, "request-1", "command-1");
            REQUIRE(operation.has_value());
            struct RiskAttempt final
            {
                ControllerBinding binding;
                Result<FrozenPlan> plan;
            };
            return RiskAttempt{
                .binding = binding,
                .plan    = prepared.store.freezePlan(
                    operation->operation.operationId,
                    operation->operation.revision,
                    lease,
                    prepared.plugin,
                    prepared.project.toolCatalogSchemaOwner,
                    prepared.planAuthority
                ),
            };
        };
        auto const underfunded = riskCase("short", 2U);
        REQUIRE_FALSE(underfunded.plan.has_value());
        CHECK(
            automationErrorKind(underfunded.plan.error())
            == AutomationErrorKind::ActionRejected
        );
        auto const exact = riskCase("exact", 3U);
        CHECK(exact.plan.has_value());

        // Read back what the two charges actually spent. Three risk units is
        // exactly one medium plan, and one mutating command is exactly one
        // mutation -- the counters are columns, so this is the database
        // answering rather than the call that wrote them.
        auto const exactRemaining = prepared.store.remainingBudget(exact.binding);
        REQUIRE(exactRemaining.has_value());
        CHECK(exactRemaining->riskUnits == 0U);
        CHECK(exactRemaining->mutations == 7U);
        auto const underfundedRemaining = prepared.store.remainingBudget(
            underfunded.binding
        );
        REQUIRE(underfundedRemaining.has_value());
        CHECK(underfundedRemaining->riskUnits == 2U);

        // TIME. Compared and never decremented, against the Operator's own
        // steady clock: a caller-supplied instant would be a caller-supplied
        // deadline. Every budgeted entry point is refused past it, including
        // one whose own arguments would have been rejected anyway -- the
        // deadline is a precondition of the call, not a late check.
        auto const timed = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-timed",
            "instance-timed",
            "target-timed",
            AgentBudget{
                .maximumToolCalls    = 8U,
                .maximumMutations    = 8U,
                .maximumObservations = 8U,
                .maximumElapsedMillis = 1'000U,
                .maximumRiskUnits     = 64U,
            }
        );
        auto const timedLease    = leaseFor(timed);
        auto const timedSnapshot = snapshotFor(timedLease);
        REQUIRE(timedSnapshot.has_value());
        auto const timedOperation = submit(timed, *timedSnapshot, "request-1", "command-1");
        REQUIRE(timedOperation.has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds{1'200});
        auto const lateSnapshot = snapshotFor(timedLease);
        REQUIRE_FALSE(lateSnapshot.has_value());
        CHECK(automationErrorKind(lateSnapshot.error()) == AutomationErrorKind::Timeout);
        auto const lateSubmit = submit(timed, *timedSnapshot, "request-2", "observe-1");
        REQUIRE_FALSE(lateSubmit.has_value());
        CHECK(automationErrorKind(lateSubmit.error()) == AutomationErrorKind::Timeout);
        auto const latePlan = prepared.store.freezePlan(
            timedOperation->operation.operationId,
            timedOperation->operation.revision,
            timedLease,
            prepared.plugin,
            prepared.project.toolCatalogSchemaOwner,
            prepared.planAuthority
        );
        REQUIRE_FALSE(latePlan.has_value());
        CHECK(automationErrorKind(latePlan.error()) == AutomationErrorKind::Timeout);
        auto const timedRemaining = prepared.store.remainingBudget(timed);
        REQUIRE(timedRemaining.has_value());
        CHECK(timedRemaining->elapsedMillisRemaining == 0U);

        // NO PROGRESS. A step makes progress when the world differs or the
        // command differs; a step that repeats both is a repetition, and the
        // Operator-owned ceiling is what stops the loop.
        auto const stuck = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-stuck",
            "instance-stuck",
            "target-stuck",
            AgentBudget{
                .maximumToolCalls    = 32U,
                .maximumMutations    = 8U,
                .maximumObservations = 8U,
                .maximumElapsedMillis = 600'000U,
                .maximumRiskUnits     = 64U,
            }
        );
        auto const stuckLease    = leaseFor(stuck);
        auto const stuckSnapshot = snapshotFor(stuckLease);
        REQUIRE(stuckSnapshot.has_value());
        auto const repetitionsOf = [&prepared](ControllerBinding const& binding)
        {
            auto const remaining = prepared.store.remainingBudget(binding);
            REQUIRE(remaining.has_value());
            return remaining->consecutiveNoProgressSteps;
        };

        // Each of these carries a FRESH client_request_id, which is what makes
        // the run a run at all: the fingerprint excludes the request id, so a
        // new one produces the identical command and buys no progress.
        REQUIRE(submit(stuck, *stuckSnapshot, "request-1", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 0U);
        REQUIRE(submit(stuck, *stuckSnapshot, "request-2", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 1U);
        REQUIRE(submit(stuck, *stuckSnapshot, "request-3", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 2U);
        REQUIRE(submit(stuck, *stuckSnapshot, "request-4", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 3U);
        auto const looped = submit(stuck, *stuckSnapshot, "request-5", "observe-1");
        REQUIRE_FALSE(looped.has_value());
        CHECK(automationErrorKind(looped.error()) == AutomationErrorKind::ActionRejected);
        CHECK(repetitionsOf(stuck) == 3U);

        // A different command against the same world is progress.
        REQUIRE(submit(stuck, *stuckSnapshot, "request-6", "command-1").has_value());
        CHECK(repetitionsOf(stuck) == 0U);
        REQUIRE(submit(stuck, *stuckSnapshot, "request-7", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 0U);
        REQUIRE(submit(stuck, *stuckSnapshot, "request-8", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 1U);

        // The same command against a different world is progress too. A second
        // Host looking at an unresolved frame reaches a different state
        // resolution, which is a different decision basis, which is what the
        // state fingerprint IS.
        auto unresolvedHost = test_support::secondObservationHost(
            prepared,
            test_support::umbraflowUnresolvedProbeFrame(),
            FrameId{909}
        );
        auto const moved = prepared.store.createSnapshot(
            stuckLease,
            prepared.plugin,
            conformance::observeOnce(unresolvedHost)
        );
        REQUIRE(moved.has_value());
        REQUIRE(moved->decisionBasisHash != stuckSnapshot->decisionBasisHash);
        REQUIRE(submit(stuck, *moved, "request-9", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 0U);
        REQUIRE(submit(stuck, *moved, "request-10", "observe-1").has_value());
        CHECK(repetitionsOf(stuck) == 1U);
    }

    // Budgets do not survive a restart, and cannot: a restart begins a new
    // session epoch, every session the previous one left behind is deactivated,
    // and no binding can be minted against a dead epoch. The rows stay as
    // spent as they were and nothing reads them again.
    TEST_CASE("an Agent budget is inert after a restart and a new session starts full")
    {
        auto const temporary = TemporaryDirectory{};
        auto const budget    = AgentBudget{
            .maximumToolCalls    = 1U,
            .maximumMutations    = 4U,
            .maximumObservations = 4U,
            .maximumElapsedMillis = 600'000U,
            .maximumRiskUnits     = 8U,
        };
        auto spentBinding = std::optional<ControllerBinding>{};
        auto pinned       = std::optional<test_support::PinnedAgentProfile>{};
        {
            auto prepared = prepareStore(temporary.path());
            pinned        = test_support::agentProfileFor(prepared, budget);
            auto const agent = test_support::addController(
                prepared,
                ControllerKind::Agent,
                SessionMode::Write,
                "session-agent",
                "instance-agent",
                "target-agent",
                budget
            );
            auto const lease = prepared.store.acquireLease(agent);
            REQUIRE(lease.has_value());
            auto const snapshot = prepared.store.createSnapshot(
                *lease,
                prepared.plugin,
                test_support::observeAgain(prepared)
            );
            REQUIRE(snapshot.has_value());
            REQUIRE(prepared.store.submitCommand(
                agent,
                test_support::command(*snapshot, "request-1"),
                test_support::toolInvocation(prepared.project, "observe-1")
            ).has_value());
            auto const remaining = prepared.store.remainingBudget(agent);
            REQUIRE(remaining.has_value());
            REQUIRE(remaining->toolCalls == 0U);
            spentBinding = agent;
        }

        auto restarted = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(restarted.has_value());

        // The binding is a value and survives the coordinator that minted it,
        // and is worth nothing: every door re-reads the pinned row.
        CHECK_FALSE(restarted->remainingBudget(*spentBinding).has_value());
        CHECK_FALSE(
            restarted->subscribe(*spentBinding, SubscriptionCursor{}, 8U).has_value()
        );
        CHECK_FALSE(restarted->bindController("session-agent").has_value());

        // Re-pinning the same session id is refused across the epoch, so a
        // restart is not a way to refresh a spent budget in place.
        auto const pin = [&pinned](std::string sessionId)
        {
            return SessionPin{
                .sessionId                 = std::move(sessionId),
                .authenticatedControllerId = "controller-1",
                .idempotencyNamespace      = "controller-1",
                .projectRegistrationHash   = pinned->manifest.projectRegistrationHash(),
                .controllerCapabilities = {std::string{conformance::k_operateCapability}},
                .controlledTargetId     = "target-agent",
                .projectInstanceKey     = "instance-agent",
                .mode                   = SessionMode::Write,
                .kind                   = ControllerKind::Agent,
            };
        };
        CHECK_FALSE(restarted->pinSession(
            pin("session-agent"),
            pinned->manifest,
            pinned->profile
        ).has_value());

        // A new session is a new binding and a full budget. That is the whole
        // of "budgets do not survive a restart": they are not carried over and
        // they are not reset either -- control resets, and budgets follow it.
        REQUIRE(restarted->pinSession(
            pin("session-agent-2"),
            pinned->manifest,
            pinned->profile
        ).has_value());
        auto const rebound = restarted->bindController("session-agent-2");
        REQUIRE(rebound.has_value());
        auto const fresh = restarted->remainingBudget(*rebound);
        REQUIRE(fresh.has_value());
        CHECK(fresh->toolCalls == budget.maximumToolCalls);
        CHECK(fresh->observations == budget.maximumObservations);
        CHECK(fresh->consecutiveNoProgressSteps == 0U);
    }

    TEST_CASE("schema-agent-a03")
    {
        auto const operatorSchema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const journalSchema   = readSchema("umbraflow-journal-v1.schema.json");
        auto const workspaceSchema = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const traceSchema     = readSchema("umbraflow-trace-v2.schema.json");
        checkStrictObject(definition(operatorSchema, "Operation"));
        checkStrictObject(definition(journalSchema, "JournalEvent"));
        auto const replay = definition(workspaceSchema, "ReplayBundle");
        checkStrictObject(replay);
        CHECK(replay.find("\"baseline_event_id\"") != std::string::npos);
        CHECK(replay.find("\"journal_prefix\"") != std::string::npos);
        CHECK(replay.find("\"operation_rows\"") != std::string::npos);
        CHECK(replay.find("\"session_manifest_hash\"") != std::string::npos);
        CHECK(traceSchema.find("\"additionalProperties\": false") != std::string::npos);

        // The trace forbids screenshot-shaped payloads by refusing the field
        // NAMES, not by listing two banned literals: searching the schema for
        // "screenshot" would be satisfied by a schema that declared such a
        // field, which is the opposite of the requirement.
        auto const fieldName = definition(traceSchema, "safe_field_name");
        CHECK(fieldName.find("\"not\"") != std::string::npos);
        CHECK(fieldName.find("screen[._-]*shot") != std::string::npos);
        CHECK(fieldName.find("frame[._-]*(bytes|data)") != std::string::npos);
    }

    TEST_CASE("contract-agent-a04")
    {
        auto const schema = readSchema("umbraflow-journal-v1.schema.json");
        auto const event  = definition(schema, "JournalEvent");
        checkStrictObject(event);
        CHECK(event.find("\"sequence\"") != std::string::npos);
        CHECK(event.find("\"prior_project_state_revision\"") != std::string::npos);
        CHECK(event.find("\"operation_id\"") != std::string::npos);
        CHECK(event.find("\"session_manifest_hash\"") != std::string::npos);
        CHECK(event.find("\"payload_schema_hash\"") != std::string::npos);
        CHECK(event.find("\"opaque_project_payload\"") != std::string::npos);
        CHECK(event.find("\"const\": 0") != std::string::npos);
        CHECK(event.find("\"type\": \"null\"") != std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());

        // The journal_events row IS this record, member for member, so the
        // schema's required list and the columns the Operator's own DDL created
        // are the same set. Everything above this line reads schema text and
        // passes whether or not the store agrees; this is the assertion that
        // ties the two together, and a column renamed on either side is red.
        auto schemaProbe = TemporaryDirectory{};
        CHECK(
            test_support::operatorTableColumns(schemaProbe.path(), "journal_events")
            == test_support::requiredMembers(event)
        );

        // Provenance is neither optional nor the caller's to invent, and the
        // schema that judges it is the framework's: JR:`JournalProvenance` is
        // fixed, so no ProjectRegistrationClaims member pins it and no project
        // supplies a validator for it. Each document below is exact JCS the
        // project's canonical validator accepts, and each violates exactly one
        // of that schema's rules -- enum, required, additionalProperties, the
        // Hash pattern, uniqueItems, the Identifier pattern. A framework check
        // that merely compared bytes against the conforming document would pass
        // these too, so the conforming document is asserted separately below.
        for (auto const violation : k_fixtureProvenanceViolations)
        {
            CAPTURE(violation);
            CHECK_FALSE(prepared.project.journalSchemaOwner.validate(
                "fixture.progress",
                canonical(prepared.project.schemaOwner, "{\"value\":1}"),
                canonical(
                    prepared.project.schemaOwner,
                    std::string{violation}
                )
            ).has_value());
        }
        CHECK(prepared.project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(prepared.project.schemaOwner, "{\"value\":1}"),
            canonical(prepared.project.schemaOwner, std::string{k_fixtureProvenance})
        ).has_value());

        // A payload the event's own schema does not accept cannot be minted
        // either, so a guessed or expected outcome has no spelling as a fact.
        CHECK_FALSE(prepared.project.journalSchemaOwner.validate(
            "fixture.progress",
            canonical(prepared.project.schemaOwner, "{\"value\":99}"),
            canonical(prepared.project.schemaOwner, std::string{k_fixtureProvenance})
        ).has_value());

        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            task::DeliveryOutcome::Delivered
        );
        auto const progressEvent = [&prepared]
        {
            auto events = std::vector<JournalAppend>{};
            events.emplace_back(
                JournalAppend{
                    .eventId = "event-1",
                    .entry = journalEntry(
                        prepared.project,
                        "fixture.progress",
                        "{\"value\":1}"
                    ),
                }
            );
            return events;
        };
        auto attempt = [&prepared, &operation](
            std::string document,
            std::vector<JournalAppend> events
        )
        {
            return prepared.store.commitReconciliation(
                prepared.plugin,
                ReconciliationCommit{
                    .operationId                  = operation.operationId,
                    .expectedOperationRevision    = operation.revision,
                    .expectedProjectStateRevision = 0U,
                    .outcome                      = reconciliationOutcome(
                        prepared,
                        operation.operationId,
                        std::move(document)
                    ),
                    .journalEvents                = std::move(events),
                }
            ).has_value();
        };

        // Rejected asserts the world did not change, and a delivered dispatch
        // is standing proof that it may have.
        CHECK_FALSE(attempt("{\"disposition\":\"rejected\"}", {}));

        // Ambiguous never established an outcome, so it may not write one.
        CHECK_FALSE(attempt("{\"disposition\":\"ambiguous\"}", progressEvent()));

        // Diverged must carry the correction that proves the divergence.
        CHECK_FALSE(attempt("{\"disposition\":\"diverged\"}", {}));

        // Continue is the one that proved something, and it is the one that
        // reaches the Journal.
        CHECK(attempt("{\"disposition\":\"continue\"}", progressEvent()));
    }

    TEST_CASE("schema-agent-a05")
    {
        auto const workspaceSchema    = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const registrationSchema = readSchema("umbraflow-project-registration-v1.schema.json");
        auto const replayGate         = definition(workspaceSchema, "ReplayGate");
        checkStrictObject(replayGate);
        CHECK(replayGate.find("\"ui_model_replay\"") != std::string::npos);
        CHECK(replayGate.find("\"project_operation_replay\"") != std::string::npos);
        CHECK(replayGate.find("\"passed\"") != std::string::npos);
        CHECK(registrationSchema.find("\"plugin_hash\"") != std::string::npos);
        CHECK(registrationSchema.find("\"project_registration_hash\"") == std::string::npos);
    }

    TEST_CASE("contract-agent-a06")
    {
        auto const workspaceSchema = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const artifactSchema  = readSchema("umbraflow-runtime-artifact-v1.schema.json");
        auto const authoringRoot   = definition(workspaceSchema, "AuthoringCapabilityRoot");
        checkStrictObject(authoringRoot);
        CHECK(authoringRoot.find("\"workspace_database\"") != std::string::npos);
        CHECK(authoringRoot.find("\"evidence_blob_root\"") != std::string::npos);
        CHECK(authoringRoot.find("\"replay_bundle_root\"") != std::string::npos);
        CHECK(artifactSchema.find("\"page_model\"") != std::string::npos);
        CHECK(artifactSchema.find("\"assets\"") != std::string::npos);
        CHECK(artifactSchema.find("screenshot") == std::string::npos);
        CHECK(artifactSchema.find("annotation_workspace") == std::string::npos);

        auto temporary = TemporaryDirectory{};
        auto const release = test_support::runtimeRelease(
            temporary.path() / "session-handoff"
        );
        auto store = OperatorCoordinator::open(temporary.path() / "production");
        REQUIRE(store.has_value());
        auto const install = [&release](ContentHash const& expected)
        {
            return RuntimeArtifactInstallRequest{
                .handoffRoot                 = release.handoffRoot,
                .expectedReleaseManifestHash = expected,
                .expectedInstalledGeneration = 0U,
            };
        };

        // The deployment principal re-verifies the release against trusted
        // metadata; it does not take the handoff's word for what it is.
        CHECK_FALSE(
            store->installRuntimeArtifact(install(hashOf("other-release"))).has_value()
        );

        // Three of the four authoring capability roots may never travel with a
        // release, so production has no path to the workspace database, the
        // evidence blobs or the replay bundles. The fourth is the exception the
        // schema pins deliberately: publication copies the committed
        // RuntimeArtifact out of candidate_workspace_root into the handoff file
        // by file, so that root's contents travel as a verified copy while the
        // root itself does not.
        auto const authoringRoots = std::array{
            std::filesystem::path{"workspace.sqlite"},
            std::filesystem::path{"evidence"} / "blob-1.png",
            std::filesystem::path{"replay"} / "bundle-1.jsonl",
        };
        for (auto const& authoringPath : authoringRoots)
        {
            test_support::writeFile(
                release.handoffRoot / authoringPath,
                "authoring bytes"
            );
            CHECK_FALSE(
                store->installRuntimeArtifact(
                    install(release.releaseManifestHash)
                ).has_value()
            );
            auto error = std::error_code{};
            static_cast<void>(std::filesystem::remove_all(
                release.handoffRoot / *authoringPath.begin(),
                error
            ));
            REQUIRE_FALSE(error);
        }

        // With nothing but the manifest-listed runtime files left, the same
        // handoff installs.
        auto const installed = store->installRuntimeArtifact(
            install(release.releaseManifestHash)
        );
        REQUIRE(installed.has_value());
        CHECK(installed->rootHash() == release.artifactRootHash);

        // The authoring side and the production side are also separate stores:
        // a handoff that sits inside the production root is refused rather than
        // read across the boundary.
        auto const nested = test_support::runtimeRelease(
            temporary.path() / "production" / "nested-handoff"
        );
        CHECK_FALSE(store->installRuntimeArtifact(
            RuntimeArtifactInstallRequest{
                .handoffRoot                 = nested.handoffRoot,
                .expectedReleaseManifestHash = nested.releaseManifestHash,
                .expectedInstalledGeneration = 1U,
            }
        ).has_value());
    }

    TEST_CASE("schema-agent-a07")
    {
        auto const schema     = readSchema("umbraflow-operator-v1.schema.json");
        auto const transition = definition(schema, "ControlTransition");
        auto const authority  = definition(schema, "DeliveryAuthority");
        checkStrictObject(transition);
        checkStrictObject(authority);
        CHECK(transition.find("\"takeover\"") != std::string::npos);
        CHECK(transition.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"session_epoch\"") != std::string::npos);
        CHECK(authority.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"authority_decision_id\"") != std::string::npos);
    }

    TEST_CASE("contract-agent-a08")
    {
        auto machine = OperationMachine{};
        REQUIRE(machine.transition(OperationEvent::ReadyWithoutApproval).has_value());
        REQUIRE(machine.transition(OperationEvent::DispatchStarted).has_value());
        auto const recovery = machine.transition(OperationEvent::PostDispatchAbort);
        REQUIRE(recovery.has_value());
        CHECK(*recovery == OperationState::Reconciling);
        CHECK(machine.mutationLocked());

        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const finding = definition(schema, "ExternalInputFinding");
        checkStrictObject(finding);
        CHECK(finding.find("\"freeze_and_reconcile\"") != std::string::npos);
        CHECK(finding.find("\"invalidated_snapshot_revision\"") != std::string::npos);
        CHECK(finding.find("\"operation_id\"") != std::string::npos);
    }
}
