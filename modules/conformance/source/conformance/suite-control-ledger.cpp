// What the Operator's ledger enforces around a project: one linearization per
// controlled target, one dispatch per Operation, one reconciliation authority,
// and a reducer input nobody outside the Operator can choose.

#include "suite-support.hpp"


#include <operator/ledger.hpp>
#include <operator/operation.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace uf::operator_runtime::conformance
{
    TEST_CASE("contract-control-c01")
    {
        auto const root = TemporaryDirectory{"c01"};
        auto prepared   = prepareStore(root.path());

        auto const takeover = prepared.store.takeoverLease(
            prepared.controller,
            "human takeover"
        );
        REQUIRE(takeover.has_value());
        CHECK(takeover->lease.fencingToken > prepared.lease.fencingToken);

        // Nothing was in flight, so the takeover resolved nothing. The count is
        // reported rather than logged because "nothing was in flight" and "one
        // effect may already have landed" are different situations.
        CHECK(takeover->resolvedDispatches == 0U);

        // The displaced lease keeps its value and loses its authority, which is
        // the only difference that matters after a takeover.
        CHECK_FALSE(prepared.store.createSnapshot(
            prepared.lease,
            prepared.plugin,
            observeAgain(prepared)
        ).has_value());
    }

    TEST_CASE("contract-control-c06")
    {
        auto const root   = TemporaryDirectory{"c06"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;

        auto const request  = command(prepared.snapshot, "request-1");
        auto const first    = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, ProjectRole::UnderTest, words.mutatingTool)
        );
        auto const repeated = prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(prepared.project, ProjectRole::UnderTest, words.mutatingTool)
        );
        REQUIRE(first.has_value());
        REQUIRE(repeated.has_value());
        CHECK(first->operation.lookup == CommandLookup::Created);
        CHECK(repeated->operation.lookup == CommandLookup::Existing);
        CHECK(first->operation.operationId == repeated->operation.operationId);
        CHECK(first->commandFingerprint == repeated->commandFingerprint);

        // Durable idempotency is by request identity, so the same identity
        // carrying a different command is a conflict rather than a second
        // Operation.
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            request,
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.otherMutatingTool
            )
        ).has_value());
    }

    TEST_CASE("contract-control-c09")
    {
        auto const root = TemporaryDirectory{"c09"};
        auto prepared   = prepareStore(root.path());
        auto const operation = readyOperation(
            prepared,
            "request-1",
            prepared.project.underTest.vocabulary.mutatingTool
        );

        auto host = deliveringHost(prepared);
        // A sink that refuses is what a real one cannot describe: the post may
        // have reached the target before the failure, and clickPoint reports one
        // Err for every phase. It is the only way to reach the outcome that
        // deliberately under-claims.
        host->refuseClicks();
        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());

        auto const report = host->deliverReport(dispatch->authority);
        REQUIRE(report.outcome() == task::DeliveryOutcome::TransportUnknown);
        CHECK_FALSE(report.reason().empty());

        // An unknown transport outcome is still an outcome: it enters
        // reconciliation rather than resolving the Operation either way.
        auto const reconciles = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            report
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
            prepared.project.underTest.vocabulary.mutatingTool
        );

        auto host           = deliveringHost(prepared);
        auto const dispatch = prepared.store.reserveDispatch(
            operation.operationId,
            operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-1"},
            std::nullopt
        );
        REQUIRE(dispatch.has_value());
        CHECK(prepared.store.reserveDispatch(
            operation.operationId,
            dispatch->operationRevision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"authority-2"},
            std::nullopt
        ).has_value() == false);
    }

    TEST_CASE("contract-control-c11")
    {
        auto const root   = TemporaryDirectory{"c11"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;
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
                    ProjectRole::UnderTest,
                    prepared.plugin,
                    operation.operationId,
                    words.continueInput
                ),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            ProjectRole::UnderTest,
                            words.progressEntry
                        ),
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
                    ProjectRole::UnderTest,
                    prepared.plugin,
                    current.operationId,
                    words.confirmedInput
                ),
                .journalEvents = {
                    JournalAppend{
                        .eventId = "event-2",
                        .entry = journalEntry(
                            prepared.project,
                            ProjectRole::UnderTest,
                            words.confirmedEntry
                        ),
                    },
                    JournalAppend{
                        .eventId = "event-1",
                        .entry = journalEntry(
                            prepared.project,
                            ProjectRole::UnderTest,
                            words.supersededEntry
                        ),
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
        auto const root   = TemporaryDirectory{"c12"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;

        // The approval edge is reached by freezing a plan whose derived risk
        // demands one, never by asking for it: no caller can name the event.
        auto const proposed = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-1"),
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.approvalRequiredPlanTool
            )
        );
        REQUIRE(proposed.has_value());
        auto const frozen = frozenPlan(prepared, proposed->operation);
        REQUIRE(frozen.has_value());
        CHECK(frozen->approvalRequired);
        CHECK(frozen->operation.state == OperationState::AwaitingApproval);

        auto const first = plannedStep(prepared, frozen->operation);
        REQUIRE(first.has_value());

        auto const approvalRequest = ApprovalRequest{
            .operationId            = proposed->operation.operationId,
            .lease                  = prepared.lease,
            .policyHash             = hashOf("policy"),
            .approverPrincipal      = "human-1",
            .approverCapabilityHash = hashOf("approval-capability"),
            .expiresAtUnixMillis    = 4'000'000'000'000U,
        };
        // Two approvals over the same pending step. The second is never
        // consumed, which is what lets the check below isolate the step binding
        // from single use: an approval that is merely spent would be refused by
        // the consumed=0 clause instead, and the case would pass with the step
        // binding removed.
        auto const spent = prepared.store.issueApproval(
            approvalRequest,
            AuthorityDecisionId{"approval-authority-1"}
        );
        auto const spare = prepared.store.issueApproval(
            approvalRequest,
            AuthorityDecisionId{"approval-authority-2"}
        );
        REQUIRE(spent.has_value());
        REQUIRE(spare.has_value());

        auto host = deliveringHost(prepared);

        // An awaiting Operation is not dispatchable without one.
        CHECK_FALSE(prepared.store.reserveDispatch(
            proposed->operation.operationId,
            first->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-unapproved"},
            std::nullopt
        ).has_value());

        auto const dispatch = prepared.store.reserveDispatch(
            proposed->operation.operationId,
            first->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-1"},
            *spent
        );
        REQUIRE(dispatch.has_value());
        CHECK(dispatch->stepIntentHash == first->stepIntentHash);
        CHECK(dispatch->authority.frozenPlanHash == frozen->planHash);
        CHECK(dispatch->decisionBasisHash == frozen->decisionBasisHash);

        // The Receipt is consumed by a context that does not hold its cycle, so
        // nothing is posted and the outcome proves the effect absent. Recording
        // it moves no fence, which is what leaves the surviving approval below
        // differing from the consumed one in the step it names and nothing else.
        auto const report = host->deliverIntoAnotherCycle(dispatch->authority);
        REQUIRE(report.outcome() == task::DeliveryOutcome::NotDelivered);
        auto const reconciling = prepared.store.recordDeliveryOutcome(
            prepared.lease,
            dispatch->operationRevision,
            report
        );
        REQUIRE(reconciling.has_value());
        CHECK(host->clicks() == 0U);

        auto const second = plannedStep(prepared, *reconciling);
        REQUIRE(second.has_value());
        CHECK(second->stepIntentHash != first->stepIntentHash);
        CHECK(second->operation.state == OperationState::AwaitingApproval);

        // The unconsumed approval names the step it was issued for, so it does
        // not carry over to the one that replaced it.
        CHECK_FALSE(prepared.store.reserveDispatch(
            proposed->operation.operationId,
            second->operation.revision,
            prepared.lease,
            host->generation(),
            AuthorityDecisionId{"dispatch-authority-2"},
            *spare
        ).has_value());
    }

    TEST_CASE("contract-control-c13")
    {
        auto const root   = TemporaryDirectory{"c13"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;

        auto const first = prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-1"),
            toolInvocation(prepared.project, ProjectRole::UnderTest, words.mutatingTool)
        );
        REQUIRE(first.has_value());

        // One mutation chain per controlled target: a second mutating command
        // is refused while the first is live, whatever tool it names.
        CHECK_FALSE(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2"),
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.otherMutatingTool
            )
        ).has_value());

        auto const cancelled = prepared.store.transitionOperation(
            first->operation.operationId,
            first->operation.revision,
            OperationSignal::Cancelled
        );
        REQUIRE(cancelled.has_value());
        CHECK(prepared.store.submitCommand(
            prepared.controller,
            command(prepared.snapshot, "request-2"),
            toolInvocation(
                prepared.project,
                ProjectRole::UnderTest,
                words.otherMutatingTool
            )
        ).has_value());
    }

    TEST_CASE("the reconciler owns the disposition, not the requester")
    {
        auto const root   = TemporaryDirectory{"disposition"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;
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
            ProjectRole::UnderTest,
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
            ProjectRole::UnderTest,
            prepared.plugin,
            operation.operationId,
            words.ambiguousInput
        );
        CHECK_FALSE(
            prepared.store.commitReconciliation(prepared.plugin, ambiguous).has_value()
        );

        // An outcome minted against another registration is refused as well,
        // however confident the conclusion it carries.
        auto foreignCommit    = confirmedCommit(
            prepared,
            operation,
            0U,
            "event-2",
            words.progressEntry
        );
        foreignCommit.outcome = reconcileOutcome(
            prepared.project,
            ProjectRole::Foreign,
            loadPlugin(prepared.project, ProjectRole::Foreign),
            operation.operationId,
            prepared.project.foreign.vocabulary.confirmedInput
        );
        CHECK_FALSE(
            prepared.store.commitReconciliation(prepared.plugin, foreignCommit).has_value()
        );
    }

    TEST_CASE("a reconcile outcome cannot be moved to another Operation")
    {
        auto const root   = TemporaryDirectory{"outcome-transplant"};
        auto prepared     = prepareStore(root.path());
        auto const& words = prepared.project.underTest.vocabulary;

        auto const first  = reconcilingOperation(
            prepared,
            "request-1",
            words.mutatingTool
        );
        auto const stolen = reconcileOutcome(
            prepared.project,
            ProjectRole::UnderTest,
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
        // The commit above moved ProjectState, so the token that opened the
        // first Operation no longer names the world and a fresh one is taken.
        prepared.snapshot    = freshSnapshot(prepared);
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
        auto const& words = prepared.project.underTest.vocabulary;
        REQUIRE(prepared.project.lastReduceInput != nullptr);

        constexpr auto eventTypeKey = std::string_view{"\"namespaced_event_type\""};

        // Provisioning reduces its own baseline event against no prior state.
        auto const baselineInput = *prepared.project.lastReduceInput;
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
        auto const commitInput = *prepared.project.lastReduceInput;
        CHECK(occurrences(commitInput, eventTypeKey) == std::size_t{1});
        CHECK(commitInput.find(words.progressEntry.payload) != std::string::npos);
        CHECK(commitInput.find(words.baselineEntry.payload) == std::string::npos);
        CHECK(commitInput.find(words.confirmedEntry.payload) == std::string::npos);
        CHECK_FALSE(commitInput.ends_with("\"prior_project_state\":null}"));
    }

    TEST_CASE("the deriver is handed an envelope no caller could have supplied")
    {
        auto const root = TemporaryDirectory{"derive-input"};
        auto prepared   = prepareStore(root.path());
        REQUIRE(prepared.project.lastDeriveInput != nullptr);

        auto const first = *prepared.project.lastDeriveInput;
        REQUIRE_FALSE(first.empty());

        // The Snapshot Coordinator assembles the envelope rather than accepting
        // one, so its five members appear in JCS order over the world the
        // composing transaction read. A caller has nowhere to put a sixth.
        auto const pendingAt  = first.find("\"pending_operation_transition\"");
        auto const pinnedAt   = first.find("\"pinned_project_artifact_identities\"");
        auto const priorAt    = first.find("\"prior_project_observation\"");
        auto const stateAt    = first.find("\"project_state\"");
        auto const snapshotAt = first.find("\"ui_snapshot\"");
        REQUIRE(pendingAt != std::string::npos);
        REQUIRE(pinnedAt != std::string::npos);
        REQUIRE(priorAt != std::string::npos);
        REQUIRE(stateAt != std::string::npos);
        REQUIRE(snapshotAt != std::string::npos);
        CHECK(pendingAt < pinnedAt);
        CHECK(pinnedAt < priorAt);
        CHECK(priorAt < stateAt);
        CHECK(stateAt < snapshotAt);

        // No prior reading is the literal null rather than an absent member, so
        // a plugin can tell it from a reading it failed to read.
        CHECK(first.find("\"prior_project_observation\":null") != std::string::npos);

        // ui_snapshot is the trailing member, so it runs to the end of the
        // envelope.
        auto const uiSnapshotOf = [](std::string const& envelope) -> std::string
        {
            auto const at = envelope.find("\"ui_snapshot\":");
            return at == std::string::npos ? std::string{} : envelope.substr(at);
        };

        // freshSnapshot captures again, so this is a second occasion over an
        // unchanged world.
        prepared.snapshot = freshSnapshot(prepared);
        auto const second = *prepared.project.lastDeriveInput;
        CHECK(second != first);

        // The next derivation is handed the reading before it, so the prior is
        // now a document; a Coordinator that dropped it would still be composing
        // snapshots and only this member would say so.
        CHECK(second.find("\"prior_project_observation\":null") == std::string::npos);

        // ui_snapshot carries the resolved world and never the occasion it was
        // captured on, which is what makes a semantically equal recapture
        // derive an identical reading rather than a new one.
        CHECK(uiSnapshotOf(second) == uiSnapshotOf(first));
        REQUIRE_FALSE(uiSnapshotOf(first).empty());
    }
}
