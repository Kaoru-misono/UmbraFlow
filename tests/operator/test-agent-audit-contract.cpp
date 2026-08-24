// The Operator audit and Agent-ceiling contracts this repository owns.
//
// A-04 is absent because its subject is: it was the shape of JR:`JournalEvent`,
// the journal_events row that stored one, and the provenance schema the
// framework applied to a Project's payload. The framework stopped interpreting
// a Project's state, so the record, its table and the reading of it are all
// deleted, and the gate was not moved anywhere.

#include <operator/ledger.hpp>
#include <operator/tool-admission-request.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <json/schema.hpp>
#include <json/value.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
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

        using test_support::hashOf;
        using test_support::prepareStore;
        using test_support::TemporaryDirectory;
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
        auto const schema = readSchema("umbraflow-operator-v1.schema.json");
        auto const budget = definition(schema, "AgentBudget");
        checkStrictObject(budget);
        CHECK(budget.find("\"maximum_tool_calls\"") != std::string::npos);
        CHECK(budget.find("\"maximum_mutations\"") != std::string::npos);
        CHECK(budget.find("\"maximum_observations\"") != std::string::npos);
        CHECK(budget.find("\"maximum_elapsed_ms\"") != std::string::npos);
        CHECK(budget.find("\"maximum_risk_units\"") != std::string::npos);

        // OP:`ProgressMarker` was the other half of A-02 and is deleted with
        // the Operation surface. Its only writer was submitCommand, so the
        // definition described a document nothing mints, and a definition with
        // neither a producer nor a reader is published as the contract without
        // being one. Asserted as an absence rather than left to a grep,
        // because re-adding it is exactly the mistake this case would
        // otherwise stop noticing.
        CHECK_MESSAGE(
            schema.find("\"ProgressMarker\"") == std::string::npos,
            "the operator protocol must not republish a definition nothing mints"
        );
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

        // Three facts about target-1 that the Agent caused none of, and that
        // between them use both surviving event kinds. The Script gives up the
        // lease the snapshot above was composed under, a Human seizes the
        // target, and that Human reports out-of-band input -- the one thing an
        // Agent most needs to hear about and can never cause.
        REQUIRE(prepared.store.releaseLease(prepared.lease).has_value());

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
        auto const finding = prepared.store.recordExternalInput(
            human,
            ExternalInputReport{
                .requiredAction = ExternalInputAction::FreezeAndReobserve,
                .reason         = "a human typed into the target",
            }
        );
        REQUIRE(finding.has_value());

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
        REQUIRE(prepared.store.releaseLease(*elsewhereLease).has_value());

        auto const read = prepared.store.subscribe(agent, base, 16U);
        REQUIRE(read.has_value());
        auto const* batch = std::get_if<SubscriptionBatch>(&*read);
        REQUIRE(batch != nullptr);

        // Exactly the three facts about this target, in order, starting at the
        // very next sequence after the snapshot's cursor. Two were caused by a
        // Script releasing control and a Human seizing it, the third by that
        // Human reporting; none by the Agent reading them. Three distinct
        // subjects, so a stream that named the wrong one is red rather than
        // merely differently ordered.
        REQUIRE(batch->events.size() == 3U);
        CHECK(batch->events[0].sequence.value == base.value + 1U);
        CHECK(batch->events[1].sequence.value == base.value + 2U);
        CHECK(batch->events[2].sequence.value == base.value + 3U);
        CHECK(batch->events[0].kind == LedgerEventKind::ControlTransitioned);
        CHECK(batch->events[1].kind == LedgerEventKind::ControlTransitioned);
        CHECK(batch->events[2].kind == LedgerEventKind::ExternalInputDetected);
        CHECK(batch->events[0].subjectId == prepared.lease.leaseId);
        CHECK(batch->events[1].subjectId == takeover->lease.leaseId);
        CHECK(batch->events[2].subjectId == finding->findingId);
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

        // The stream's third kind was OperationStateChanged, carrying the
        // committed state as a LedgerEvent detail, and both went with the
        // Operation record. A Tool call's state changes publish nothing here
        // and the event has no detail member to carry one, so the two kinds
        // above are the whole vocabulary and both are exercised.
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
            auto const worldScope = ObservedInstanceWorldScope::run(
                controlledTargetId,
                1
            );
            REQUIRE(worldScope.has_value());
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
                .worldScope                = *worldScope,
            };
        };

        // A ProjectInstance nothing has pinned yet. The refusals below have to
        // fail for the reason under test, and an instance that already carries
        // an active write session refuses a second one on its own -- which is
        // exactly what masked all three of these when they named instance-1.
        REQUIRE(prepared.store.provisionProjectInstance(
            prepared.project.registration,
            "instance-pins"
        ).has_value());

        // The ceilings are the exact bytes the manifest attests to. A profile
        // verified against one manifest cannot be presented with another, and
        // bytes that do not hash to agent_profile_hash never become a profile
        // at all.
        //
        // What is NOT asserted here any more: that the controller kind decides
        // whether a session carries ceilings. Every session declares a budget,
        // whoever controls it, so an unbudgeted session is not a shape the
        // types can spell and a budgeted Script is an ordinary session rather
        // than a refusal.
        auto const narrowed = test_support::agentProfileFor(
            prepared,
            AgentBudget{
                .maximumToolCalls    = 4U,
                .maximumMutations    = 2U,
                .maximumObservations = 4U,
                .maximumElapsedMillis = 60'000U,
                .maximumRiskUnits     = 32U,
            }
        );
        CHECK_FALSE(prepared.store.pinSession(
            pin("session-crossed", "instance-pins", "target-pins", ControllerKind::Agent),
            prepared.manifest,
            narrowed.profile
        ).has_value());

        // The positive control for the refusal above: the same instance and
        // the same target, pinned with the profile this manifest attests to.
        // Without it, a refusal is consistent with an instance nothing could
        // be pinned to at all.
        REQUIRE(prepared.store.pinSession(
            pin("session-pins", "instance-pins", "target-pins", ControllerKind::Agent),
            narrowed.manifest,
            narrowed.profile
        ).has_value());

        CHECK_FALSE(AgentProfile::verifyExact(
            narrowed.manifest,
            "agent-profile.json",
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
                prepared.project.registration,
                prepared.project.toolCatalogSchemaOwner,
                prepared.project.observedInstanceIdentitySchemas,
                test_support::observeAgain(prepared)
            );
        };

        // One Tool call admitted under a named controller's own binding and
        // lease. It builds what tool-call-fixture.hpp builds but hands back the
        // Result rather than requiring it, because every ceiling below is
        // proved by a refusal and a refusal has to be a value this case can
        // read the error kind off.
        //
        // Everything the coordinate is derived from is a function of the
        // request key and the tool, so presenting the same pair again is the
        // same call rather than a second one -- which is what the replay below
        // stands on. The key must therefore be unique per controller here: all
        // of these bindings authenticate as controller-1, and a root request is
        // keyed by that namespace and the key alone.
        auto const admit = [&prepared](
            ControllerBinding const& binding,
            ControlLease const& lease,
            std::string_view requestKey,
            std::string_view toolName
        )
        {
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"agent-budget"})"
            );
            REQUIRE(preimage.has_value());
            auto const root = ToolRootRequestIdentity::create(
                std::string{binding.controllerId()},
                std::string{requestKey},
                *std::move(preimage)
            );
            REQUIRE(root.has_value());
            auto const invocation = test_support::toolInvocation(
                prepared.project,
                std::string{toolName}
            );
            auto const call = test_support::toolCallAt(
                *root,
                nullptr,
                1U,
                ToolExecutionIdentity{
                    .runIdentity                 = prepared.manifest.hash(),
                    .frameworkReleaseIdentity    = prepared.runtimeArtifactRootHash,
                    .toolRuntimeProtocolIdentity = hashOf("a02-protocol"),
                    .environmentIdentity         = hashOf("a02-environment"),
                },
                invocation
            );
            REQUIRE(call.has_value());

            // Read off the descriptor exactly as admission reads it, so this
            // case cannot present a mutating tool as read-only either.
            auto mutation = std::optional<ToolAdmissionRequest::Mutation>{};
            if (invocation.descriptor().mutability == ToolMutability::Mutating)
            {
                mutation = ToolAdmissionRequest::Mutation{
                    .effects         = {test_support::routineToolEffect(
                        prepared.project,
                        std::string{toolName}
                    )},
                };
            }
            return prepared.store.admitToolCall(ToolAdmissionRequest{
                .controller      = binding,
                .lease           = lease,
                .root            = *root,
                .call            = *call,
                .policyAuthority = prepared.policyAuthority,
                .mutation        = std::move(mutation),
            });
        };

        // The two catalog names this case calls. A fixture tool's namespace
        // is its registration's plugin_id, so they are composed from the
        // project prepareStore registered rather than respelled per call.
        auto const observeTool = prepared.project.toolName("observe-1");
        auto const commandTool = prepared.project.toolName("command-1");

        // ACTION. One admitted call, and the second is refused by the column's
        // own CHECK rather than by a comparison beside it.
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
        auto const callsLease = leaseFor(calls);
        auto const admitted   = admit(calls, callsLease, "calls-1", observeTool);
        REQUIRE(admitted.has_value());
        auto const spent = admit(calls, callsLease, "calls-2", observeTool);
        REQUIRE_FALSE(spent.has_value());
        CHECK(automationErrorKind(spent.error()) == AutomationErrorKind::ActionRejected);
        auto const callsRemaining = prepared.store.remainingBudget(calls);
        REQUIRE(callsRemaining.has_value());
        CHECK(callsRemaining->toolCalls == 0U);

        // The columns are separate spends and not one counter under five
        // names: admitting a read-only Project Tool charges the tool-call
        // column and nothing else, so the observation ceiling is demonstrably
        // not what refused the second call.
        CHECK(callsRemaining->observations == callsBudget.maximumObservations);

        // Exhaustion refuses new work; it does not un-record accepted work.
        // Presenting the admitted call's own coordinate again rejoins its
        // existing admission -- the same attempt at the same history revision
        // -- and costs nothing, because the counter records what the ledger
        // admitted and this was charged when it was admitted. A second attempt
        // would carry the next attempt number and would have to find a
        // tool-call budget that is already zero.
        auto const replay = admit(calls, callsLease, "calls-1", observeTool);
        REQUIRE(replay.has_value());
        CHECK(replay->attemptNumber() == admitted->attemptNumber());
        CHECK(replay->historyRevision() == admitted->historyRevision());
        auto const afterReplay = prepared.store.remainingBudget(calls);
        REQUIRE(afterReplay.has_value());
        CHECK(afterReplay->toolCalls == 0U);

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
        // effects the caller proposed, so a zero mutation ceiling refuses a
        // mutating tool while leaving every read-only one available.
        auto const mutationsBudget = AgentBudget{
            .maximumToolCalls    = 8U,
            .maximumMutations    = 0U,
            .maximumObservations = 8U,
            .maximumElapsedMillis = 600'000U,
            .maximumRiskUnits     = 64U,
        };
        auto const mutations = test_support::addController(
            prepared,
            ControllerKind::Agent,
            SessionMode::Write,
            "session-mutations",
            "instance-mutations",
            "target-mutations",
            mutationsBudget
        );
        auto const mutationsLease  = leaseFor(mutations);
        auto const refusedMutation = admit(
            mutations,
            mutationsLease,
            "mutations-1",
            commandTool
        );
        REQUIRE_FALSE(refusedMutation.has_value());
        CHECK(
            automationErrorKind(refusedMutation.error())
            == AutomationErrorKind::ActionRejected
        );
        REQUIRE(
            admit(mutations, mutationsLease, "mutations-2", observeTool).has_value()
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

        // RISK. The column survives on the budget row with no writer left: the
        // one charge priced a frozen plan's derived risk and went with
        // freezePlan and the Operation surface, and Tool admission prices
        // effects against the PolicyArtifact rather than against a counter.
        // What is provable here is therefore the absence itself -- the
        // mutating and read-only calls above left the column exactly where the
        // profile put it -- and the day admission starts charging it, this is
        // the line that says so and demands a ceiling case beside it.
        auto const mutationsRemaining = prepared.store.remainingBudget(mutations);
        REQUIRE(mutationsRemaining.has_value());
        CHECK(mutationsRemaining->riskUnits == mutationsBudget.maximumRiskUnits);

        // TIME. Compared and never decremented, against the Operator's own
        // steady clock: a caller-supplied instant would be a caller-supplied
        // deadline. Both budgeted doors are refused past it -- the snapshot
        // coordinator and Tool admission -- because the deadline is a
        // precondition of the call and not a late check.
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
        auto const timedLease = leaseFor(timed);
        REQUIRE(snapshotFor(timedLease).has_value());
        REQUIRE(admit(timed, timedLease, "timed-1", observeTool).has_value());
        std::this_thread::sleep_for(std::chrono::milliseconds{1'200});
        auto const lateSnapshot = snapshotFor(timedLease);
        REQUIRE_FALSE(lateSnapshot.has_value());
        CHECK(automationErrorKind(lateSnapshot.error()) == AutomationErrorKind::Timeout);
        auto const lateCall = admit(timed, timedLease, "timed-2", observeTool);
        REQUIRE_FALSE(lateCall.has_value());
        CHECK(automationErrorKind(lateCall.error()) == AutomationErrorKind::Timeout);
        auto const timedRemaining = prepared.store.remainingBudget(timed);
        REQUIRE(timedRemaining.has_value());
        CHECK(timedRemaining->elapsedMillisRemaining == 0U);

        // NO PROGRESS. The ceiling went with its only writer. submitCommand was
        // what compared a step's state and command fingerprints and moved
        // consecutive_no_progress_steps; the column and the remaining-budget
        // member survive on the agent_budgets row and no door writes either, so
        // an Agent's stopping condition is the four ceilings above and nothing
        // else. A case that still counted repetitions would be reading a
        // counter nothing increments -- a green line dressed as a ceiling.
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
                prepared.project.registration,
                prepared.project.toolCatalogSchemaOwner,
                prepared.project.observedInstanceIdentitySchemas,
                test_support::observeAgain(prepared)
            );
            REQUIRE(snapshot.has_value());

            // One admitted Tool call is the whole of this budget's action
            // ceiling; the row read back below is what proves it was charged,
            // so the admission itself is wanted only for its effect.
            static_cast<void>(test_support::startToolCall(
                prepared,
                agent,
                *lease,
                "restart-request-1",
                prepared.project.toolName("observe-1"),
                test_support::ToolCallReach::Admitted
            ));
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
            auto const worldScope = ObservedInstanceWorldScope::run(
                "target-agent",
                1
            );
            REQUIRE(worldScope.has_value());
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
                .worldScope             = *worldScope,
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
        auto const workspaceSchema = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const traceSchema     = readSchema("umbraflow-trace-v2.schema.json");
        auto const replay = definition(workspaceSchema, "ReplayBundle");
        checkStrictObject(replay);
        CHECK(replay.find("\"observations\"") != std::string::npos);
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

    TEST_CASE("schema-agent-a05")
    {
        auto const workspaceSchema    = readSchema("umbraflow-annotation-workspace-v2.schema.json");
        auto const registrationSchema = readSchema("umbraflow-project-registration-v4.schema.json");
        auto const attestationSchema = readSchema("umbraflow-project-attestation-v2.schema.json");
        auto const replayGate         = definition(workspaceSchema, "ReplayGate");
        checkStrictObject(replayGate);
        CHECK(replayGate.find("\"ui_model_replay\"") != std::string::npos);
        CHECK(replayGate.find("\"project_operation_replay\"") != std::string::npos);
        CHECK(replayGate.find("\"passed\"") != std::string::npos);
        // The registration document states the code it was deployed as, in
        // its one closure, and never its own root: a document that named the
        // digest of its own bytes would be attesting to itself.
        CHECK(registrationSchema.find("\"tool_closure\"") != std::string::npos);
        CHECK(registrationSchema.find("\"module_manifest_hash\"") != std::string::npos);
        CHECK(registrationSchema.find("\"plugin_environment_hash\"") != std::string::npos);
        CHECK(registrationSchema.find("\"project_registration_hash\"") == std::string::npos);

        auto const compiled = json::Schema::compile(json::Schema::Document{
            .label      = "umbraflow-project-attestation-v2.schema.json",
            .exactBytes = attestationSchema,
        });
        REQUIRE(compiled.has_value());
        auto const digest = std::string(64U, 'a');
        auto valid = std::format(
            R"json({{"set_version":2,"predecessor_set_id":null,"bundle_root_hash":"{}","plugin_id":"fixture.project","attestations":[{{"attestation_id":"{}","requirement_id":"A-05","bundle_root_hash":"{}","plugin_id":"fixture.project","project_registration_hash":"{}","content_pack_hash":"{}","recognition_pack_hash":"{}","source_snapshot_hash":"{}","build_recipe_hash":"{}","passed":true,"report_hash":"{}","attestor_principal":"fixture","capability_hash":"{}","created_at":"2026-08-20T00:00:00Z"}}]}})json",
            digest,
            digest,
            digest,
            digest,
            digest,
            digest,
            digest,
            digest,
            digest,
            digest
        );
        auto const accepts = [&compiled](std::string_view bytes)
        {
            auto const parsed = json::parse(bytes);
            return parsed.has_value() && compiled->validate(*parsed).has_value();
        };
        auto const replaced = [](std::string source, std::string_view from, std::string_view to)
        {
            auto const at = source.find(from);
            REQUIRE(at != std::string::npos);
            source.replace(at, from.size(), to);
            return source;
        };
        CHECK(accepts(valid));
        CHECK_FALSE(accepts(replaced(valid, "\"set_version\":2", "\"set_version\":1")));
        CHECK_FALSE(accepts(replaced(
            valid,
            std::format("\"project_registration_hash\":\"{}\",", digest),
            ""
        )));
        CHECK_FALSE(accepts(replaced(
            valid,
            std::format("\"project_registration_hash\":\"{}\",", digest),
            std::format(
                "\"project_registration_hash\":\"{}\",\"plugin_hash\":\"{}\",",
                digest,
                digest
            )
        )));
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
        checkStrictObject(transition);
        CHECK(transition.find("\"takeover\"") != std::string::npos);
        CHECK(transition.find("\"fencing_token\"") != std::string::npos);
        CHECK(transition.find("\"session_epoch\"") != std::string::npos);
        CHECK(transition.find("\"prior_lease_id\"") != std::string::npos);
        CHECK(transition.find("\"new_lease_id\"") != std::string::npos);

        // The other half of A-07, and the reason the takeover race matters: a
        // displaced controller's fence is still named by an authority a Host
        // may still be holding, so the two documents state the same three
        // values and the ledger is the only mint of the second.
        auto const authority = definition(schema, "DeliveryAuthority");
        checkStrictObject(authority);
        CHECK(authority.find("\"session_epoch\"") != std::string::npos);
        CHECK(authority.find("\"fencing_token\"") != std::string::npos);
        CHECK(authority.find("\"lease_id\"") != std::string::npos);
    }

    TEST_CASE("contract-agent-a08")
    {
        auto const schema  = readSchema("umbraflow-operator-v1.schema.json");
        auto const finding = definition(schema, "ExternalInputFinding");
        checkStrictObject(finding);
        CHECK(finding.find("\"freeze_and_reconcile\"") != std::string::npos);
        CHECK(finding.find("\"detected_after_cursor\"") != std::string::npos);
        CHECK(finding.find("\"invalidated_snapshot_revision\"") != std::string::npos);

        // OP:`OperationMachine` was the other half of A-08 and is deleted: the
        // freeze it drove was a transition of the Operation record. What
        // survives is the finding itself -- a durable statement of which
        // snapshot revision stopped being safe to act on -- and the half of it
        // this case can pin is that the statement is the ledger's and not the
        // reporter's.
        auto temporary = TemporaryDirectory{};
        auto prepared  = prepareStore(temporary.path());
        auto const human = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Write,
            "session-human",
            "instance-human",
            "target-1"
        );

        // Everything above reads schema text and passes whether or not the
        // store agrees. What a finding invalidates is read back inside the
        // recording transaction from the row it just wrote, never returned
        // from the locals that were bound, so the value below is the ledger's
        // answer about this target's own revision line.
        auto const report = ExternalInputReport{
            .requiredAction = ExternalInputAction::FreezeAndReconcile,
            .reason         = "a human typed into the target",
        };
        auto const first = prepared.store.recordExternalInput(human, report);
        REQUIRE(first.has_value());
        CHECK(first->invalidatedSnapshotRevision == prepared.snapshot.snapshotRevision);

        // The positive control: a later snapshot moves what the next finding
        // invalidates, so the revision is read from the target rather than
        // fixed at whatever the first finding happened to see.
        auto const again = test_support::freshSnapshot(prepared);
        REQUIRE(again.snapshotRevision > prepared.snapshot.snapshotRevision);
        auto const second = prepared.store.recordExternalInput(human, report);
        REQUIRE(second.has_value());
        CHECK(second->invalidatedSnapshotRevision == again.snapshotRevision);
        CHECK(second->detectedAfterCursor > first->detectedAfterCursor);
    }
}
