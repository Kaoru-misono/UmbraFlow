// Work package 9 of the cycle SPI plan: reducer execution stays pure and gains
// a trusted commit context, and a Journal batch reaches the Journal only
// through a call-bound proposal and one final CAS.
//
// Three defects the plan's section 14.2 audited are the subject. The reduce
// envelope carries `commit_context` and calls its batch prospective, because
// reduction commits nothing. A proposal is persisted against the exact call
// tree and outcome revision, cannot commit while a referenced effect is
// uncertain, and is published by one CAS that re-verifies those identities and
// the prior revision. And section 5.4's dependent-Journal-commit clause is
// enforced, with the one live mutation chain the commit belongs to excluded
// from the barrier rather than frozen by it.

#include <operator/ledger.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-descriptor.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::hashOf;
        using test_support::TemporaryDirectory;

        // doctest's message macro binds its message expression tighter than a
        // conditional operator, so the text is built here rather than inline.
        template <typename T>
        [[nodiscard]]
        auto failureText(Result<T> const& result) -> std::string
        {
            return result.has_value()
                ? std::string{}
                : std::string{result.error().message()};
        }

        [[nodiscard]]
        auto rootFor(std::string_view requestKey) -> ToolRootRequestIdentity
        {
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"journal-commit"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                std::string{requestKey},
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            return *std::move(root);
        }

        [[nodiscard]]
        auto executionIdentity() -> ToolExecutionIdentity
        {
            return ToolExecutionIdentity{
                .runIdentity                 = hashOf("journal-run"),
                .frameworkReleaseIdentity    = hashOf("journal-framework"),
                .toolRuntimeProtocolIdentity = hashOf("journal-protocol"),
                .environmentIdentity         = hashOf("journal-environment"),
            };
        }

        // One call taken to its dispatch boundary, and the token that crossed
        // it. Both are kept because a case that proposes needs the coordinate
        // and a case that completes needs the token.
        struct RunningCall final
        {
            ToolCallPositionIdentity call;
            ToolCallDispatch         dispatch;
        };

        [[nodiscard]]
        auto startCall(
            test_support::PreparedStore& prepared,
            ToolRootRequestIdentity const& root,
            ToolCallIssuingContext& context,
            ValidatedToolInvocation const& invocation,
            std::optional<ToolDelegationGrant> delegation = std::nullopt
        ) -> RunningCall
        {
            auto call = context.issue(invocation);
            REQUIRE_MESSAGE(call.has_value(), failureText(call));
            auto mutation = std::optional<ToolAdmissionRequest::Mutation>{};
            if (invocation.descriptor().mutability == ToolMutability::Mutating)
            {
                mutation = ToolAdmissionRequest::Mutation{
                    .policyAuthority = prepared.policyAuthority,
                    .effects         = {test_support::routineToolEffect(
                        prepared.project,
                        invocation.toolName()
                    )},
                };
            }
            auto admitted = prepared.store.admitToolCall(ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = *call,
                .mutation   = std::move(mutation),
                .delegation = std::move(delegation),
            });
            REQUIRE_MESSAGE(admitted.has_value(), failureText(admitted));
            auto dispatch = prepared.store.beginToolCallDispatch(*admitted);
            REQUIRE_MESSAGE(dispatch.has_value(), failureText(dispatch));
            return RunningCall{*std::move(call), *std::move(dispatch)};
        }

        auto completeCall(
            test_support::PreparedStore& prepared,
            RunningCall const& running,
            ToolCallCompletion const& completion
        ) -> void
        {
            auto completed = prepared.store.completeToolCallDispatch(
                running.dispatch,
                completion
            );
            REQUIRE_MESSAGE(completed.has_value(), failureText(completed));
        }

        [[nodiscard]]
        auto canonicalOf(std::string_view exactJcs) -> CanonicalJson
        {
            auto value = CanonicalJson::parseExact(std::string{exactJcs});
            REQUIRE(value.has_value());
            return *std::move(value);
        }

        // One `fixture.confirmed` batch. The fixture reducer answers
        // {"revision":1} when it sees that event type in its prospective batch
        // and {"revision":0} otherwise, so a published commit is visible as a
        // state that moved rather than as a row count.
        [[nodiscard]]
        auto confirmedBatch(
            test_support::ProjectFixture const& project,
            std::string eventId
        ) -> std::vector<JournalAppend>
        {
            auto events = std::vector<JournalAppend>{};
            events.emplace_back(JournalAppend{
                .eventId = std::move(eventId),
                .entry   = test_support::journalEntry(
                    project,
                    "fixture.confirmed",
                    R"({"value":1})"
                ),
            });
            return events;
        }

        [[nodiscard]]
        auto expectedBaselineEnvelope() -> std::string
        {
            return std::string{
                       R"({"commit_context":{"next_revision":0,)"
                       R"("prior_revision":null},"prior_project_state":null,)"
                       R"("prospective_journal_batch":[)"
                       R"({"namespaced_event_type":"fixture.baseline",)"
                       R"("opaque_project_payload":{"kind":"baseline"},)"
                       R"("provenance":)"
                   }
                + std::string{test_support::k_fixtureProvenance} + "}]}";
        }

        [[nodiscard]]
        auto expectedCommitEnvelope() -> std::string
        {
            return std::string{
                       R"({"commit_context":{"next_revision":1,)"
                       R"("prior_revision":0},)"
                       R"("prior_project_state":{"revision":0},)"
                       R"("prospective_journal_batch":[)"
                       R"({"namespaced_event_type":"fixture.confirmed",)"
                       R"("opaque_project_payload":{"value":1},)"
                       R"("provenance":)"
                   }
                + std::string{test_support::k_fixtureProvenance} + "}]}";
        }

        // The Project catalog the two nested cases drive: `command-1` declares
        // `observe-1` and `command-2` as its children, which the shared
        // fixture's catalog does not. The bytes are the registration's own, so
        // the owner is bound to the same tool_catalog_hash; what a trusted
        // deployment callback declares over them is what a deployment declares.
        [[nodiscard]]
        auto delegatingCatalog(test_support::ProjectFixture const& project)
            -> ProjectToolCatalogSchemaOwner
        {
            auto parent = project.toolCatalogSchemaOwner.describe(
                project.toolName("command-1")
            );
            auto reader = project.toolCatalogSchemaOwner.describe(
                project.toolName("observe-1")
            );
            auto mutatingChild = project.toolCatalogSchemaOwner.describe(
                project.toolName("command-2")
            );
            REQUIRE(parent.has_value());
            REQUIRE(reader.has_value());
            REQUIRE(mutatingChild.has_value());
            parent->childEffects = ChildEffectDeclaration{
                .childToolNames = {
                    project.toolName("command-2"),
                    project.toolName("observe-1"),
                },
                .maximumChildSurface    = ToolSurface::Semantic,
                .maximumChildMutability = ToolMutability::Mutating,
                .maximumChildRisk       = Risk::High,
                .maximumChildCalls      = 4U,
            };
            auto tools = std::vector<ToolCatalogEntry>{
                ToolCatalogEntry{
                    .name       = project.toolName("command-1"),
                    .descriptor = *std::move(parent),
                },
                ToolCatalogEntry{
                    .name       = project.toolName("command-2"),
                    .descriptor = *std::move(mutatingChild),
                },
                ToolCatalogEntry{
                    .name       = project.toolName("observe-1"),
                    .descriptor = *std::move(reader),
                },
            };
            auto catalog = ProjectToolCatalogSchemaOwner::create(
                project.registration,
                project.toolCatalogBytes,
                [tools = std::move(tools)]() -> Result<std::vector<ToolCatalogEntry>>
                {
                    return tools;
                },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
            REQUIRE_MESSAGE(catalog.has_value(), failureText(catalog));
            return *std::move(catalog);
        }

        [[nodiscard]]
        auto invocationOf(
            test_support::ProjectFixture const& project,
            ProjectToolCatalogSchemaOwner const& catalog,
            std::string_view localName
        ) -> ValidatedToolInvocation
        {
            auto invocation = catalog.validate(
                project.toolName(localName),
                test_support::canonical(project.schemaOwner, R"({"value":1})")
            );
            REQUIRE_MESSAGE(invocation.has_value(), failureText(invocation));
            return *std::move(invocation);
        }
    }

    // Section 8's envelope, byte for byte, at both revisions a fold happens at.
    // The commit context is TRUSTED: nothing in either call supplies it, and
    // both revisions are derived from the project_state row the Operator holds.
    TEST_CASE("the reduce envelope carries the trusted commit context")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());

        // Provisioning folds the baseline: no prior state, and therefore no
        // prior revision, at the revision every later one is derived from.
        CHECK(
            prepared.project.documentInputLog->lastReduceInput()
            == expectedBaselineEnvelope()
        );

        auto const root = rootFor("commit-context-request");
        auto context    = ToolCallIssuingContext::forRoot(root, executionIdentity());
        auto const invocation = test_support::toolInvocation(
            prepared.project,
            prepared.project.toolName("observe-1")
        );
        auto const running = startCall(prepared, root, context, invocation);
        auto const proposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            running.call,
            JournalBatchProposal{
                .events = confirmedBatch(prepared.project, "commit-context-1"),
            }
        );
        REQUIRE_MESSAGE(proposal.has_value(), failureText(proposal));
        CHECK(proposal->lookup == ToolIdentityLookup::Created);
        CHECK(proposal->priorProjectStateRevision == 0U);

        // Proposing is not committing. Nothing has folded since provisioning,
        // and neither the Journal nor the state has moved.
        CHECK(
            prepared.project.documentInputLog->lastReduceInput()
            == expectedBaselineEnvelope()
        );

        // The identity is the content address of everything the proposer
        // stated, so the same batch from the same incarnation rejoins the
        // stored row instead of freezing a second copy of itself.
        auto const again = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            running.call,
            JournalBatchProposal{
                .events = confirmedBatch(prepared.project, "commit-context-1"),
            }
        );
        REQUIRE_MESSAGE(again.has_value(), failureText(again));
        CHECK(again->lookup == ToolIdentityLookup::Existing);
        CHECK(again->proposalIdentity == proposal->proposalIdentity);

        completeCall(
            prepared,
            running,
            ToolCallCompletion::confirmed(canonicalOf(R"({"observed":true})"))
        );
        auto const published = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_MESSAGE(published.has_value(), failureText(published));
        CHECK(published->revision == 1U);
        CHECK(published->lastJournalSequence == 1U);

        // The publishing fold saw the state it is derived from and the two
        // revisions that name where it sits in the chain.
        CHECK(
            prepared.project.documentInputLog->lastReduceInput()
            == expectedCommitEnvelope()
        );

        // The whole retained Journal still folds to the published state, batch
        // by batch, which is the property a refold exists to answer.
        auto const refolded = prepared.store.refoldProjectState(
            prepared.project.registration,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            "instance-1"
        );
        REQUIRE_MESSAGE(refolded.has_value(), failureText(refolded));
        CHECK(refolded->journalEventCount == 2U);
        CHECK(refolded->storedStateHash == refolded->refoldedStateHash);
        CHECK(refolded->refoldedCanonicalPayload == R"({"revision":1})");
    }

    // The final CAS re-verifies the prior revision the proposal was frozen
    // against, and a proposal publishes once.
    TEST_CASE("a Journal batch publishes once, against the revision it froze")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const root = rootFor("stale-revision-request");
        auto context    = ToolCallIssuingContext::forRoot(root, executionIdentity());
        auto const invocation = test_support::toolInvocation(
            prepared.project,
            prepared.project.toolName("observe-1")
        );

        // Two proposals frozen against the same revision 0, from two calls of
        // one run. The second is stale the instant the first publishes.
        auto const first = startCall(prepared, root, context, invocation);
        auto const firstProposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            first.call,
            JournalBatchProposal{
                .events = confirmedBatch(prepared.project, "stale-1"),
            }
        );
        REQUIRE_MESSAGE(firstProposal.has_value(), failureText(firstProposal));
        completeCall(
            prepared,
            first,
            ToolCallCompletion::confirmed(canonicalOf(R"({"observed":true})"))
        );

        auto const second = startCall(prepared, root, context, invocation);
        auto const secondProposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            second.call,
            JournalBatchProposal{
                .events = confirmedBatch(prepared.project, "stale-2"),
            }
        );
        REQUIRE_MESSAGE(secondProposal.has_value(), failureText(secondProposal));
        CHECK(secondProposal->priorProjectStateRevision == 0U);
        completeCall(
            prepared,
            second,
            ToolCallCompletion::confirmed(canonicalOf(R"({"observed":true})"))
        );

        auto const published = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            firstProposal->proposalIdentity
        );
        REQUIRE_MESSAGE(published.has_value(), failureText(published));
        CHECK(published->revision == 1U);

        auto const again = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            firstProposal->proposalIdentity
        );
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().message().contains("already published"));

        auto const stale = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            secondProposal->proposalIdentity
        );
        REQUIRE_FALSE(stale.has_value());
        CHECK(stale.error().message().contains("was frozen against ProjectState"));
        CHECK(stale.error().message().contains("now at revision 1"));
    }

    // The call-tree half of the same CAS: a proposal names the outcome revision
    // of the dispatch it was made inside, and an incarnation that re-entered
    // that dispatch reaches a different one.
    TEST_CASE("a Journal batch proposal is bound to the incarnation that made it")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const root = rootFor("incarnation-request");
        auto context    = ToolCallIssuingContext::forRoot(root, executionIdentity());
        auto const invocation = test_support::toolInvocation(
            prepared.project,
            prepared.project.toolName("observe-1")
        );
        auto const running = startCall(prepared, root, context, invocation);
        auto const proposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            running.call,
            JournalBatchProposal{
                .events = confirmedBatch(prepared.project, "incarnation-1"),
            }
        );
        REQUIRE_MESSAGE(proposal.has_value(), failureText(proposal));

        // A batch is published by the confirmed outcome of the call that made
        // it, so the running handler cannot publish its own facts.
        auto const early = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_FALSE(early.has_value());
        CHECK(early.error().message().contains("which is dispatching"));

        // The incarnation that made the proposal dies inside its dispatch and
        // the row is re-entered. Its completion now lands one revision past the
        // one the proposal named.
        auto const reentered = prepared.store.reenterToolCallDispatch(
            prepared.controller,
            prepared.lease,
            root,
            running.call
        );
        REQUIRE_MESSAGE(reentered.has_value(), failureText(reentered));
        auto const answered = prepared.store.completeToolCallDispatch(
            *reentered,
            ToolCallCompletion::confirmed(canonicalOf(R"({"observed":true})"))
        );
        REQUIRE_MESSAGE(answered.has_value(), failureText(answered));

        auto const orphaned = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_FALSE(orphaned.has_value());
        CHECK(orphaned.error().message().contains("names outcome revision"));
    }

    // Section 7: a proposal cannot commit while a referenced effect -- here the
    // proposing handler's own child -- is uncertain, and cannot commit once
    // that effect has been resolved either, because resolving it replaces the
    // outcome the facts were computed from.
    TEST_CASE("a Journal batch cannot outrun the outcomes it interprets")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const catalog = delegatingCatalog(prepared.project);
        auto const root    = rootFor("uncertain-effect-request");
        auto context = ToolCallIssuingContext::forRoot(root, executionIdentity());

        auto const handler = startCall(
            prepared,
            root,
            context,
            invocationOf(prepared.project, catalog, "command-1")
        );
        auto const grant = prepared.store.issueToolDelegationGrant(handler.call);
        REQUIRE_MESSAGE(grant.has_value(), failureText(grant));

        auto handlerContext = ToolCallIssuingContext::forHandler(handler.call);
        auto const effect   = startCall(
            prepared,
            root,
            handlerContext,
            invocationOf(prepared.project, catalog, "command-2"),
            *grant
        );
        completeCall(
            prepared,
            effect,
            ToolCallCompletion::possible(
                canonicalOf(R"({"reason":"the click may have landed"})")
            )
        );

        auto const proposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            handler.call,
            JournalBatchProposal{
                .events          = confirmedBatch(prepared.project, "uncertain-1"),
                .referencedCalls = {effect.call.identity()},
            }
        );
        REQUIRE_MESSAGE(proposal.has_value(), failureText(proposal));
        completeCall(
            prepared,
            handler,
            ToolCallCompletion::confirmed(canonicalOf(R"({"handled":true})"))
        );

        auto const frozen = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_FALSE(frozen.has_value());
        CHECK(
            frozen.error().message().contains(
                "ControlledTarget mutation is frozen by Tool call"
            )
        );
        CHECK(frozen.error().message().contains("in state possible"));

        // Reconciliation resolves the uncertainty and, in doing so, replaces
        // the outcome the batch was computed from. The facts have to be
        // computed again rather than published over a world that moved.
        auto const resolved = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            root,
            effect.call,
            [](ToolCallPositionIdentity const&)
            {
                return ToolCallReconciliation::confirmed(
                    canonicalOf(R"({"delivered":true})"),
                    canonicalOf(R"({"snapshot_ref":"post-reconcile"})")
                );
            }
        );
        REQUIRE_MESSAGE(resolved.has_value(), failureText(resolved));
        CHECK(resolved->state == ToolCallState::Confirmed);

        auto const moved = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_FALSE(moved.has_value());
        CHECK(
            moved.error().message().contains(
                "which is now confirmed at revision"
            )
        );
    }

    // Section 5.4's dependent-Journal-commit clause, and the exclusion that
    // says what "dependent" means. A possible mutating call anywhere else on
    // the controlled target freezes the commit; the one live mutation chain the
    // commit is part of does not, because a dispatching ancestor is not an
    // uncertain delivery.
    TEST_CASE("a frozen ControlledTarget refuses a dependent Journal commit")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const root = rootFor("frozen-target-request");
        auto context    = ToolCallIssuingContext::forRoot(root, executionIdentity());

        // A settled mutation the batch interprets, so the refusal below cannot
        // be the referenced-effect test wearing another name.
        auto const settled = startCall(
            prepared,
            root,
            context,
            test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName("command-1")
            )
        );
        completeCall(
            prepared,
            settled,
            ToolCallCompletion::confirmed(canonicalOf(R"({"delivered":true})"))
        );

        auto const reader = startCall(
            prepared,
            root,
            context,
            test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName("observe-1")
            )
        );
        auto const proposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            reader.call,
            JournalBatchProposal{
                .events          = confirmedBatch(prepared.project, "frozen-1"),
                .referencedCalls = {settled.call.identity()},
            }
        );
        REQUIRE_MESSAGE(proposal.has_value(), failureText(proposal));
        completeCall(
            prepared,
            reader,
            ToolCallCompletion::confirmed(canonicalOf(R"({"observed":true})"))
        );

        // An unrelated mutation on the same target ends uncertain. The batch
        // never mentions it, and it freezes the commit all the same.
        auto const uncertain = startCall(
            prepared,
            root,
            context,
            test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName("command-2")
            )
        );
        completeCall(
            prepared,
            uncertain,
            ToolCallCompletion::possible(
                canonicalOf(R"({"reason":"the second click may have landed"})")
            )
        );

        auto const refused = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(
            refused.error().message().contains(
                "ControlledTarget mutation is frozen by Tool call"
            )
        );
        CHECK(refused.error().message().contains("in state possible"));
    }

    // The other direction of the same clause. A handler proposing from inside a
    // mutating parent that is still dispatching is inside the one live mutation
    // chain, not dependent on a frozen one, and its commit is admitted.
    TEST_CASE("a commit inside its own live mutation chain is admitted")
    {
        auto temporary = TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const catalog = delegatingCatalog(prepared.project);
        auto const root    = rootFor("live-chain-request");
        auto context = ToolCallIssuingContext::forRoot(root, executionIdentity());

        auto const parent = startCall(
            prepared,
            root,
            context,
            invocationOf(prepared.project, catalog, "command-1")
        );
        auto const grant = prepared.store.issueToolDelegationGrant(parent.call);
        REQUIRE_MESSAGE(grant.has_value(), failureText(grant));

        auto handlerContext = ToolCallIssuingContext::forHandler(parent.call);
        auto const child    = startCall(
            prepared,
            root,
            handlerContext,
            invocationOf(prepared.project, catalog, "observe-1"),
            *grant
        );
        auto const proposal = prepared.store.proposeJournalBatch(
            prepared.controller,
            root,
            child.call,
            JournalBatchProposal{
                .events = confirmedBatch(prepared.project, "live-chain-1"),
            }
        );
        REQUIRE_MESSAGE(proposal.has_value(), failureText(proposal));
        completeCall(
            prepared,
            child,
            ToolCallCompletion::confirmed(canonicalOf(R"({"observed":true})"))
        );

        // The parent is a mutating call in flight on this very target. It is
        // the chain this commit belongs to, so the barrier does not see it.
        auto const parentState = prepared.store.replayToolCall(root, parent.call);
        REQUIRE(parentState.has_value());
        CHECK(parentState->state == ToolCallState::Dispatching);

        auto const published = prepared.store.publishJournalProposal(
            prepared.controller,
            prepared.lease,
            prepared.project.journalSchemaOwner,
            prepared.generation,
            proposal->proposalIdentity
        );
        REQUIRE_MESSAGE(published.has_value(), failureText(published));
        CHECK(published->revision == 1U);
    }
}
