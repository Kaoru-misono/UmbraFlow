// What the Operator's ledger enforces around a project: one linearization per
// controlled target, one dispatch per Operation, one reconciliation authority,
// and a reducer input nobody outside the Operator can choose.

#include "harness.hpp"

#include <operator-contract/project-under-test.hpp>

#include <operator/ledger.hpp>
#include <operator/operation.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace uf::operator_runtime::contract
{
    TEST_CASE("contract-control-c01")
    {
        auto const root = TemporaryDirectory{"c01"};
        auto prepared   = prepareStore(root.path());

        auto const takeover = prepared.store.takeoverLease(
            "session-1",
            "human takeover"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->fencingToken > prepared.lease.fencingToken);

        // The displaced lease keeps its value and loses its authority, which is
        // the only difference that matters after a takeover.
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            hashOf("stale-snapshot")
        ).has_value());
    }

    TEST_CASE("contract-control-c06")
    {
        auto const root   = TemporaryDirectory{"c06"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.vocabulary;

        auto const request  = command(prepared.snapshot, "request-1");
        auto const first    = prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, words.mutatingTool)
        );
        auto const repeated = prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, words.mutatingTool)
        );
        REQUIRE(first.has_value());
        REQUIRE(repeated.has_value());
        CHECK(first->lookup == CommandLookup::Created);
        CHECK(repeated->lookup == CommandLookup::Existing);
        CHECK(first->operationId == repeated->operationId);

        // Durable idempotency is by request identity, so the same identity
        // carrying a different command is a conflict rather than a second
        // Operation.
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            request,
            toolInvocation(prepared.project, words.otherMutatingTool)
        ).has_value());
    }

    TEST_CASE("contract-control-c09")
    {
        auto const root = TemporaryDirectory{"c09"};
        auto prepared   = prepareStore(root.path());
        auto const operation = readyOperation(
            prepared,
            "request-1",
            prepared.project.vocabulary.mutatingTool
        );

        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());

        // An unknown transport outcome is still an outcome: it enters
        // reconciliation rather than resolving the Operation either way.
        auto const reconciles = prepared.store.recordDeliveryOutcome(
            operation.operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::TransportUnknown
        );
        REQUIRE(reconciles.has_value());
        CHECK(reconciles->state == OperationState::Reconciling);
        CHECK(reconciles->hasDispatched);
    }

    TEST_CASE("contract-control-c10")
    {
        auto const root = TemporaryDirectory{"c10"};
        auto prepared   = prepareStore(root.path());
        auto const operation = readyOperation(
            prepared,
            "request-1",
            prepared.project.vocabulary.mutatingTool
        );

        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-1",
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK(prepared.store.reserveDispatch(
            operation.operationId,
            dispatch->operationRevision,
            prepared.lease,
            hashOf("decision"),
            hashOf("plan"),
            hashOf("step"),
            "authority-2",
            std::nullopt
        ).has_value() == false);
    }

    TEST_CASE("contract-control-c11")
    {
        auto const root   = TemporaryDirectory{"c11"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.vocabulary;
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            words.mutatingTool
        );

        auto const progress = prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = operation.operationId,
                .expectedOperationRevision    = operation.revision,
                .expectedProjectStateRevision = 0U,
                .outcome                      = reconcileOutcome(
                    prepared.project,
                    prepared.plugin,
                    operation.operationId,
                    words.continueInput
                ),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(prepared.project, words.progressEntry),
                    },
                },
            }
        );
        REQUIRE(progress.has_value());
        CHECK(progress->state == OperationState::Reconciling);

        // A commit against the revision the previous one consumed is refused,
        // so two reconcilers cannot both believe they moved the same step.
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(
                prepared,
                operation,
                1U,
                "event-2",
                words.confirmedEntry
            )
        ).has_value());

        auto const current = StoredOperation{
            .operationId = operation.operationId,
            .lookup      = CommandLookup::Existing,
            .state         = progress->state,
            .revision      = progress->revision,
            .planFrozen    = progress->planFrozen,
            .hasDispatched = progress->hasDispatched,
        };

        // An event id already in the Journal takes the whole commit down with
        // it, including the entry beside it that would otherwise have been new.
        CHECK_FALSE(prepared.store.commitReconciliation(
            prepared.plugin,
            ReconciliationCommit{
                .operationId                  = current.operationId,
                .expectedOperationRevision    = current.revision,
                .expectedProjectStateRevision = 1U,
                .outcome                      = reconcileOutcome(
                    prepared.project,
                    prepared.plugin,
                    current.operationId,
                    words.confirmedInput
                ),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-2",
                        .entry   = journalEntry(prepared.project, words.confirmedEntry),
                    },
                    JournalAppend{
                        .eventId = "event-1",
                        .entry   = journalEntry(prepared.project, words.supersededEntry),
                    },
                },
            }
        ).has_value());

        auto const confirmed = prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, current, 1U, "event-2", words.confirmedEntry)
        );
        REQUIRE(confirmed.has_value());
        CHECK(confirmed->state == OperationState::Confirmed);
        CHECK(confirmed->revision > current.revision);
    }

    TEST_CASE("contract-control-c12")
    {
        auto const root = TemporaryDirectory{"c12"};
        auto prepared   = prepareStore(root.path());

        auto operation = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, prepared.project.vocabulary.mutatingTool)
        );
        REQUIRE(operation.has_value());
        operation = prepared.store.transitionOperation(
            operation->operationId,
            operation->revision,
            OperationEvent::ApprovalRequired
        );
        REQUIRE(operation.has_value());

        auto const planHash = hashOf("plan");
        auto const stepHash = hashOf("step");
        auto const approval = prepared.store.issueApproval(
            ApprovalRequest{
                .operationId            = operation->operationId,
                .lease                  = prepared.lease,
                .frozenPlanHash         = planHash,
                .stepIntentHash         = stepHash,
                .decisionBasisHash      = hashOf("decision"),
                .effectEnvelopeHash     = hashOf("effects"),
                .policyHash             = hashOf("policy"),
                .approverPrincipal      = "human-1",
                .approverCapabilityHash = hashOf("approval-capability"),
                .expiresAtUnixMillis    = 4'000'000'000'000U,
            },
            "approval-authority-1"
        );
        REQUIRE(approval.has_value());

        // The token names the step it approved, so it does not carry over to a
        // different one.
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation->operationId,
            operation->revision,
            prepared.lease,
            hashOf("decision"),
            planHash,
            hashOf("a-different-step"),
            "dispatch-authority-invalid",
            *approval
        ).has_value());

        auto const dispatch = prepared.store.reserveDispatch(
            operation->operationId,
            operation->revision,
            prepared.lease,
            hashOf("decision"),
            planHash,
            stepHash,
            "dispatch-authority-1",
            *approval
        );
        REQUIRE(dispatch.has_value());
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            operation->operationId,
            dispatch->dispatchSequence,
            dispatch->operationRevision,
            DeliveryOutcome::NotDelivered
        );
        REQUIRE(reconciling.has_value());

        // Single use: the next step of the same Operation needs its own
        // approval, and the spent token does not answer for it.
        auto const awaiting = prepared.store.transitionOperation(
            operation->operationId,
            reconciling->revision,
            OperationEvent::NextStepApprovalRequired
        );
        REQUIRE(awaiting.has_value());
        CHECK_FALSE(prepared.store.reserveDispatch(
            operation->operationId,
            awaiting->revision,
            prepared.lease,
            hashOf("decision"),
            planHash,
            stepHash,
            "dispatch-authority-2",
            *approval
        ).has_value());
    }

    TEST_CASE("contract-control-c13")
    {
        auto const root   = TemporaryDirectory{"c13"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.vocabulary;

        auto first = prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, words.mutatingTool)
        );
        REQUIRE(first.has_value());

        // One mutation chain per controlled target: a second mutating command
        // is refused while the first is live, whatever tool it names.
        CHECK_FALSE(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, words.otherMutatingTool)
        ).has_value());

        first = prepared.store.transitionOperation(
            first->operationId,
            first->revision,
            OperationEvent::Cancelled
        );
        REQUIRE(first.has_value());
        CHECK(prepared.store.createOrLoadOperation(
            command(prepared.snapshot, "request-2"),
            toolInvocation(prepared.project, words.otherMutatingTool)
        ).has_value());
    }

    TEST_CASE("the reconciler owns the disposition, not the requester")
    {
        auto const root   = TemporaryDirectory{"disposition"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.vocabulary;
        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            words.mutatingTool
        );

        // A proposal that concluded Rejected cannot be committed as anything
        // else, because the disposition is read out of that document by the
        // reconcile authority rather than supplied beside it.
        auto rejected    = confirmedCommit(
            prepared,
            operation,
            0U,
            "event-1",
            words.progressEntry
        );
        rejected.outcome = reconcileOutcome(
            prepared.project,
            prepared.plugin,
            operation.operationId,
            words.rejectedInput
        );
        CHECK_FALSE(
            prepared.store.commitReconciliation(prepared.plugin, rejected).has_value()
        );

        auto ambiguous    = confirmedCommit(
            prepared,
            operation,
            0U,
            "event-1",
            words.progressEntry
        );
        ambiguous.outcome = reconcileOutcome(
            prepared.project,
            prepared.plugin,
            operation.operationId,
            words.ambiguousInput
        );
        CHECK_FALSE(
            prepared.store.commitReconciliation(prepared.plugin, ambiguous).has_value()
        );

        // An outcome minted against another registration is refused as well,
        // however confident the conclusion it carries.
        auto const foreign      = projectUnderTest(ProjectRole::Foreign);
        auto foreignCommit      = confirmedCommit(
            prepared,
            operation,
            0U,
            "event-2",
            words.progressEntry
        );
        foreignCommit.outcome   = reconcileOutcome(
            foreign,
            loadPlugin(foreign),
            operation.operationId,
            foreign.vocabulary.confirmedInput
        );
        CHECK_FALSE(
            prepared.store.commitReconciliation(prepared.plugin, foreignCommit).has_value()
        );
    }

    TEST_CASE("a reconcile outcome cannot be moved to another Operation")
    {
        auto const root   = TemporaryDirectory{"outcome-transplant"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.vocabulary;

        auto const first  = reconcilingOperation(
            prepared,
            "request-1",
            words.mutatingTool
        );
        auto const stolen = reconcileOutcome(
            prepared.project,
            prepared.plugin,
            first.operationId,
            words.confirmedInput
        );
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, first, 0U, "event-1", words.progressEntry)
        ).has_value());

        // The first Operation is terminal, so the mutation chain is free for a
        // second. Same registration, same schema, same disposition document --
        // only the Operation the conclusion was reached about separates them.
        auto const second    = reconcilingOperation(
            prepared,
            "request-2",
            words.otherMutatingTool
        );
        auto transplanted    = confirmedCommit(
            prepared,
            second,
            1U,
            "event-2",
            words.confirmedEntry
        );
        transplanted.outcome = stolen;
        CHECK_FALSE(
            prepared.store.commitReconciliation(prepared.plugin, transplanted).has_value()
        );
    }

    TEST_CASE("the reducer is handed exactly the Journal prefix that is appended")
    {
        auto const root   = TemporaryDirectory{"reducer-input"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.vocabulary;
        REQUIRE(prepared.project.observedReduceInput != nullptr);

        constexpr auto eventTypeKey = std::string_view{"\"namespaced_event_type\""};

        // Provisioning reduces its own baseline event against no prior state.
        auto const baselineInput = *prepared.project.observedReduceInput;
        CHECK(occurrences(baselineInput, eventTypeKey) == std::size_t{1});
        CHECK(baselineInput.find(words.baselineEntry.payload) != std::string::npos);
        CHECK(baselineInput.find(words.provenance) != std::string::npos);
        CHECK(baselineInput.ends_with("\"prior_project_state\":null}"));

        auto const operation = reconcilingOperation(
            prepared,
            "request-1",
            words.mutatingTool
        );
        REQUIRE(prepared.store.commitReconciliation(
            prepared.plugin,
            confirmedCommit(prepared, operation, 0U, "event-1", words.progressEntry)
        ).has_value());

        // The envelope is a function of the events this commit named and the
        // ProjectState the database already held, so a caller who wanted the
        // reducer to see something else has nowhere to put it: neither the
        // baseline payload nor any entry the commit did not name is in there.
        auto const commitInput = *prepared.project.observedReduceInput;
        CHECK(occurrences(commitInput, eventTypeKey) == std::size_t{1});
        CHECK(commitInput.find(words.progressEntry.payload) != std::string::npos);
        CHECK(commitInput.find(words.baselineEntry.payload) == std::string::npos);
        CHECK(commitInput.find(words.confirmedEntry.payload) == std::string::npos);
        CHECK_FALSE(commitInput.ends_with("\"prior_project_state\":null}"));
    }
}
