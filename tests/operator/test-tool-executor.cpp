#include <operator/tool-executor.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::toolCallAt;

        [[nodiscard]]
        auto testHash(std::string_view text) -> ContentHash
        {
            auto hash = sha256(std::as_bytes(std::span{text}));
            REQUIRE(hash.has_value());
            return *hash;
        }

        [[nodiscard]]
        auto frameworkCall(
            ToolRootRequestIdentity const& root,
            std::string_view runName
        ) -> ToolCallPositionIdentity
        {
            auto catalog   = FrameworkToolCatalogOwner::create();
            auto arguments = CanonicalJson::parseExact("{}");
            REQUIRE(catalog.has_value());
            REQUIRE(arguments.has_value());
            auto invocation = catalog->validate(
                "framework.screen.observe",
                std::move(*arguments)
            );
            REQUIRE(invocation.has_value());
            auto call = toolCallAt(
                root,
                nullptr,
                1U,
                ToolExecutionIdentity{
                    .runIdentity                 = testHash(runName),
                    .frameworkReleaseIdentity    = testHash("executor-framework"),
                    .toolRuntimeProtocolIdentity = testHash("executor-protocol"),
                    .environmentIdentity         = testHash("executor-environment"),
                },
                *invocation
            );
            REQUIRE(call.has_value());
            return *std::move(call);
        }

        [[nodiscard]]
        auto mutatingProjectCall(
            ToolRootRequestIdentity const& root,
            test_support::ProjectFixture const& project,
            std::string_view runName
        ) -> ToolCallPositionIdentity
        {
            auto invocation = test_support::toolInvocation(
                project,
                project.toolName("command-1")
            );
            REQUIRE(invocation.descriptor().mutability == ToolMutability::Mutating);
            auto call = toolCallAt(
                root,
                nullptr,
                1U,
                ToolExecutionIdentity{
                    .runIdentity                 = testHash(runName),
                    .frameworkReleaseIdentity    = testHash("executor-framework"),
                    .toolRuntimeProtocolIdentity = testHash("executor-protocol"),
                    .environmentIdentity         = testHash("executor-environment"),
                },
                invocation
            );
            REQUIRE(call.has_value());
            return *std::move(call);
        }

        [[nodiscard]]
        auto toolRoot(std::string key) -> ToolRootRequestIdentity
        {
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"mutating executor test"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                "controller-1",
                std::move(key),
                std::move(*preimage)
            );
            REQUIRE(root.has_value());
            return *std::move(root);
        }
    }

    TEST_CASE("Tool executor invokes one read-only provider and replays without authority")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto preimage  = CanonicalJson::parseExact(
            R"({"objective":"executor-observe"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            "controller-1",
            "executor-request",
            std::move(*preimage)
        );
        REQUIRE(root.has_value());
        auto call = frameworkCall(*root, "executor-run");
        auto result = CanonicalJson::parseExact(
            R"({"snapshot_ref":"snapshot-1"})"
        );
        REQUIRE(result.has_value());
        auto providerCalls = uint64{};
        {
            auto executor = ToolRuntimeExecutor{prepared.store};
            auto replay = executor.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = *root,
                    .call       = call,
                },
                [&providerCalls, &result](ToolCallPositionIdentity const& presented)
                {
                    ++providerCalls;
                    CHECK(presented.toolName() == "framework.screen.observe");
                    return ToolCallCompletion::confirmed(*result);
                }
            );
            REQUIRE(replay.has_value());
            REQUIRE(replay->payload.has_value());
            CHECK(replay->state == ToolCallState::Confirmed);
            CHECK(replay->payload->bytes() == result->bytes());
            CHECK(providerCalls == 1U);

            auto failurePreimage = CanonicalJson::parseExact(
                R"({"objective":"executor-failure"})"
            );
            REQUIRE(failurePreimage.has_value());
            auto failureRoot = ToolRootRequestIdentity::create(
                "controller-1",
                "executor-failure-request",
                std::move(*failurePreimage)
            );
            REQUIRE(failureRoot.has_value());
            auto failureCall = frameworkCall(*failureRoot, "executor-failure-run");
            auto failed = executor.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = *failureRoot,
                    .call       = failureCall,
                },
                [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
                {
                    return fail(
                        AutomationErrorKind::CaptureUnavailable,
                        "capture provider refused"
                    );
                }
            );
            REQUIRE(failed.has_value());
            REQUIRE(failed->payload.has_value());
            CHECK(failed->state == ToolCallState::TerminalFailure);
            CHECK(
                failed->payload->bytes()
                == R"({"failure_response":"abort","kind":"capture_unavailable","message":"capture provider refused"})"
            );
            auto refusedReplayExecutions = uint64{};
            auto replayedFailure = executor.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = *failureRoot,
                    .call       = failureCall,
                },
                [&refusedReplayExecutions](ToolCallPositionIdentity const&)
                {
                    ++refusedReplayExecutions;
                    auto replacement = CanonicalJson::parseExact("{}");
                    REQUIRE(replacement.has_value());
                    return ToolCallCompletion::confirmed(*replacement);
                }
            );
            REQUIRE(replayedFailure.has_value());
            CHECK(replayedFailure->state == ToolCallState::TerminalFailure);
            CHECK(refusedReplayExecutions == 0U);
        }

        {
            auto released = std::move(prepared.store);
        }
        auto restarted = OperatorCoordinator::open(
            temporary.path() / "production"
        );
        REQUIRE(restarted.has_value());
        auto executor = ToolRuntimeExecutor{*restarted};
        auto replay = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = *root,
                .call       = call,
            },
            [&providerCalls](ToolCallPositionIdentity const&)
            {
                ++providerCalls;
                auto changed = CanonicalJson::parseExact(
                    R"({"snapshot_ref":"snapshot-2"})"
                );
                REQUIRE(changed.has_value());
                return ToolCallCompletion::confirmed(*changed);
            }
        );
        REQUIRE(replay.has_value());
        REQUIRE(replay->payload.has_value());
        CHECK(replay->payload->bytes() == result->bytes());
        CHECK(providerCalls == 1U);

        auto missingPreimage = CanonicalJson::parseExact("{}");
        REQUIRE(missingPreimage.has_value());
        auto missingRoot = ToolRootRequestIdentity::create(
            "controller-1",
            "executor-missing-provider",
            std::move(*missingPreimage)
        );
        REQUIRE(missingRoot.has_value());
        auto missingCall = frameworkCall(*missingRoot, "executor-missing-run");
        auto refused = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = *missingRoot,
                .call       = missingCall,
            },
            {}
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("requires a provider"));
        auto absent = restarted->replayToolCall(*missingRoot, missingCall);
        REQUIRE_FALSE(absent.has_value());
        CHECK(absent.error().message().contains("root is not durable"));

    }

    TEST_CASE(
        "possible mutating Tool freezes the target until evidence reconciles it"
    )
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto firstRoot = toolRoot("mutating-possible");
        auto firstCall = mutatingProjectCall(
            firstRoot,
            prepared.project,
            "mutating-possible-run"
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto executor      = ToolRuntimeExecutor{prepared.store};
        auto providerCalls = uint64{};
        auto possible = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = firstRoot,
                .call       = firstCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            // Stated by the provider rather than converted from a failure.
            // This call is COMPOSED, so nothing turns its failure into
            // uncertainty; `possible` stays admissible for it because a
            // handler that replayed to an unresolved child inherits that
            // child's uncertainty and has nothing else to report.
            [&providerCalls](ToolCallPositionIdentity const&)
                -> Result<ToolCallCompletion>
            {
                ++providerCalls;
                auto explanation = CanonicalJson::parseExact(
                    R"({"kind":"io_failure","reason":"a child delivery never resolved"})"
                );
                REQUIRE(explanation.has_value());
                return ToolCallCompletion::possible(*explanation);
            }
        );
        auto const possibleWhy = possible.has_value()
            ? std::string{}
            : possible.error().message();
        REQUIRE_MESSAGE(possible.has_value(), possibleWhy);
        REQUIRE(possible->payload.has_value());
        CHECK(possible->state == ToolCallState::Possible);
        CHECK(providerCalls == 1U);
        CHECK(possible->payload->bytes().contains("io_failure"));

        auto secondRoot = toolRoot("mutating-blocked");
        auto secondCall = mutatingProjectCall(
            secondRoot,
            prepared.project,
            "mutating-blocked-run"
        );
        auto blockedProviderCalls = uint64{};
        auto blocked = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = secondRoot,
                .call       = secondCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [&blockedProviderCalls](ToolCallPositionIdentity const&)
            {
                ++blockedProviderCalls;
                auto result = CanonicalJson::parseExact(R"({"delivered":true})");
                REQUIRE(result.has_value());
                return ToolCallCompletion::confirmed(*result);
            }
        );
        REQUIRE_FALSE(blocked.has_value());
        CHECK(blocked.error().message().contains("frozen by Tool call"));
        CHECK(blockedProviderCalls == 0U);

        auto oldOperationBlocked = prepared.store.submitCommand(
            prepared.controller,
            CommandRequest{
                .snapshotToken        = prepared.snapshot.token,
                .idempotencyNamespace = "controller-1",
                .clientRequestId      = "old-operation-during-tool-barrier",
            },
            test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName("command-1")
            )
        );
        REQUIRE_FALSE(oldOperationBlocked.has_value());
        CHECK(oldOperationBlocked.error().message().contains(
            "frozen by Tool call"
        ));

        // Observation remains available while mutation is frozen so a caller
        // can gather the evidence reconciliation needs.
        auto observeRoot = toolRoot("read-only-during-barrier");
        auto observeCall = frameworkCall(observeRoot, "barrier-observe-run");
        auto observation = CanonicalJson::parseExact(
            R"({"snapshot_ref":"barrier-snapshot"})"
        );
        REQUIRE(observation.has_value());
        auto observed = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = observeRoot,
                .call       = observeCall,
            },
            [&observation](ToolCallPositionIdentity const&)
            {
                return ToolCallCompletion::confirmed(*observation);
            }
        );
        REQUIRE(observed.has_value());
        CHECK(observed->state == ToolCallState::Confirmed);

        auto reconciledResult = CanonicalJson::parseExact(
            R"({"delivered":true})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"snapshot_ref":"fresh-reconciliation-snapshot"})"
        );
        REQUIRE(reconciledResult.has_value());
        REQUIRE(evidence.has_value());

        // A query that answers about a call which is not uncertain is never
        // asked: the Coordinator proves the row possible before it interrogates
        // anything, so a querier cannot choose which call an answer is about.
        auto settledQueries = uint64{};
        auto refusedSettled = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            observeRoot,
            observeCall,
            [&settledQueries, &reconciledResult, &evidence](
                ToolCallPositionIdentity const&
            )
            {
                ++settledQueries;
                return ToolCallReconciliation::confirmed(
                    *reconciledResult,
                    *evidence
                );
            }
        );
        REQUIRE_FALSE(refusedSettled.has_value());
        CHECK(refusedSettled.error().message().contains(
            "requires a mutating call"
        ));
        CHECK(settledQueries == 0U);

        // A query that cannot answer resolves nothing, and the row is left
        // exactly as uncertain as it was.
        auto failedQuery = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [](ToolCallPositionIdentity const&)
                -> Result<ToolCallReconciliation>
            {
                return fail(
                    AutomationErrorKind::CaptureUnavailable,
                    "the reconciliation query could not reach the target"
                );
            }
        );
        REQUIRE_FALSE(failedQuery.has_value());
        CHECK(failedQuery.error().message().contains("could not reach"));
        auto stillPossible = prepared.store.replayToolCall(firstRoot, firstCall);
        REQUIRE(stillPossible.has_value());
        CHECK(stillPossible->state == ToolCallState::Possible);

        auto queriedCalls = uint64{};
        auto reconciled   = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [&queriedCalls, &reconciledResult, &evidence, &firstCall](
                ToolCallPositionIdentity const& queried
            )
            {
                ++queriedCalls;
                CHECK(queried.identity() == firstCall.identity());
                return ToolCallReconciliation::confirmed(
                    *reconciledResult,
                    *evidence
                );
            }
        );
        REQUIRE(reconciled.has_value());
        REQUIRE(reconciled->evidence.has_value());
        CHECK(reconciled->state == ToolCallState::Confirmed);
        CHECK(reconciled->evidence->bytes() == evidence->bytes());
        CHECK(queriedCalls == 1U);

        // Reconciliation is the transition out of uncertainty, so a second one
        // has nothing to leave. It is refused rather than rejoined, and the
        // query is not asked again.
        auto repeated = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [&queriedCalls, &reconciledResult, &evidence](
                ToolCallPositionIdentity const&
            )
            {
                ++queriedCalls;
                return ToolCallReconciliation::confirmed(
                    *reconciledResult,
                    *evidence
                );
            }
        );
        REQUIRE_FALSE(repeated.has_value());
        CHECK(repeated.error().message().contains(
            "Only a possible mutating Tool call may be reconciled"
        ));
        CHECK(queriedCalls == 1U);

        auto unblocked = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = secondRoot,
                .call       = secondCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [&blockedProviderCalls](ToolCallPositionIdentity const&)
            {
                ++blockedProviderCalls;
                auto result = CanonicalJson::parseExact(R"({"delivered":true})");
                REQUIRE(result.has_value());
                return ToolCallCompletion::confirmed(*result);
            }
        );
        REQUIRE(unblocked.has_value());
        CHECK(unblocked->state == ToolCallState::Confirmed);
        CHECK(blockedProviderCalls == 1U);
    }

    TEST_CASE("mutating Tool executor dispatches only after a call-bound approval")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto root      = toolRoot("executor-high-risk-approval");
        auto call = mutatingProjectCall(
            root,
            prepared.project,
            "executor-high-risk-approval-run"
        );
        auto effect        = test_support::routineToolEffect(prepared.project);
        effect.risk        = Risk::High;
        auto effects       = std::vector{effect};
        auto executor      = ToolRuntimeExecutor{prepared.store};
        auto providerCalls = uint64{};
        auto approver = test_support::addController(
            prepared,
            ControllerKind::Human,
            SessionMode::Read,
            "executor-approver-session",
            "executor-approver-instance",
            prepared.controller.controlledTargetId(),
            std::nullopt,
            "executor-approver",
            {"approve"}
        );
        auto provider = [&providerCalls](ToolCallPositionIdentity const&)
            -> Result<ToolCallCompletion>
        {
            ++providerCalls;
            auto result = CanonicalJson::parseExact(R"({"delivered":true})");
            REQUIRE(result.has_value());
            return ToolCallCompletion::confirmed(*result);
        };

        auto refused = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            provider
        );
        REQUIRE_FALSE(refused.has_value());
        CHECK(refused.error().message().contains("requires approval"));
        CHECK(providerCalls == 0U);

        auto approval = prepared.store.issueToolApproval(
            prepared.controller,
            prepared.lease,
            approver,
            root,
            call,
            prepared.planAuthority,
            effects,
            ToolApprovalRequest{
                .approverCapability = "approve",
                .expiresAtUnixMillis = static_cast<uint64>(
                    std::numeric_limits<int64>::max()
                ),
            },
            AuthorityDecisionId{"executor-approval-decision"}
        );
        REQUIRE(approval.has_value());
        auto approvals = std::vector{*approval};
        auto invoked = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                    .approvals     = approvals,
                },
            },
            provider
        );
        REQUIRE(invoked.has_value());
        CHECK(invoked->state == ToolCallState::Confirmed);
        CHECK(providerCalls == 1U);

        auto replayed = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            provider
        );
        REQUIRE(replayed.has_value());
        CHECK(replayed->state == ToolCallState::Confirmed);
        CHECK(providerCalls == 1U);
    }

    TEST_CASE("terminally unresolved mutating Tool keeps the target frozen")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto firstRoot = toolRoot("terminally-unresolved");
        auto firstCall = mutatingProjectCall(
            firstRoot,
            prepared.project,
            "terminally-unresolved-run"
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto executor = ToolRuntimeExecutor{prepared.store};
        auto error = CanonicalJson::parseExact(R"({"delivery":"unknown"})");
        REQUIRE(error.has_value());
        auto possible = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = firstRoot,
                .call       = firstCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            // Stated by the provider rather than converted from a failure:
            // this call is COMPOSED, so the executor leaves its terminal
            // failure terminal. `possible` stays admissible for it, because a
            // handler that replayed to an unresolved child inherits that
            // child's uncertainty and has nothing else to report.
            [&error](ToolCallPositionIdentity const&)
            {
                return ToolCallCompletion::possible(*error);
            }
        );
        auto const possibleWhy = possible.has_value()
            ? std::string{}
            : possible.error().message();
        REQUIRE_MESSAGE(possible.has_value(), possibleWhy);
        CHECK(possible->state == ToolCallState::Possible);

        auto explanation = CanonicalJson::parseExact(
            R"({"reason":"evidence remained inconclusive"})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"snapshot_ref":"latest-known-snapshot"})"
        );
        REQUIRE(explanation.has_value());
        REQUIRE(evidence.has_value());
        auto unresolved = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [&explanation, &evidence](ToolCallPositionIdentity const&)
            {
                return ToolCallReconciliation::terminallyUnresolved(
                    *explanation,
                    *evidence
                );
            }
        );
        REQUIRE(unresolved.has_value());
        CHECK(unresolved->state == ToolCallState::TerminallyUnresolved);

        auto secondRoot = toolRoot("blocked-after-terminally-unresolved");
        auto secondCall = mutatingProjectCall(
            secondRoot,
            prepared.project,
            "blocked-after-terminally-unresolved-run"
        );
        auto blocked = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = secondRoot,
                .call       = secondCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [](ToolCallPositionIdentity const&)
            {
                auto result = CanonicalJson::parseExact(R"({"delivered":true})");
                REQUIRE(result.has_value());
                return ToolCallCompletion::confirmed(*result);
            }
        );
        REQUIRE_FALSE(blocked.has_value());
        CHECK(blocked.error().message().contains("terminally_unresolved"));
    }

    TEST_CASE("legacy active mutation and Tool mutation cannot run beside each other")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto invocation = test_support::toolInvocation(
            prepared.project,
            prepared.project.toolName("command-1")
        );
        auto accepted = prepared.store.submitCommand(
            prepared.controller,
            CommandRequest{
                .snapshotToken        = prepared.snapshot.token,
                .idempotencyNamespace = "controller-1",
                .clientRequestId      = "legacy-active-mutation",
            },
            invocation
        );
        REQUIRE(accepted.has_value());

        auto root = toolRoot("tool-beside-legacy-mutation");
        auto call = mutatingProjectCall(
            root,
            prepared.project,
            "tool-beside-legacy-mutation-run"
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto providerCalls = uint64{};
        auto executor      = ToolRuntimeExecutor{prepared.store};
        auto blocked = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [&providerCalls](ToolCallPositionIdentity const&)
            {
                ++providerCalls;
                auto result = CanonicalJson::parseExact(R"({"delivered":true})");
                REQUIRE(result.has_value());
                return ToolCallCompletion::confirmed(*result);
            }
        );
        REQUIRE_FALSE(blocked.has_value());
        CHECK(blocked.error().message().contains(
            "non-terminal mutating Operation"
        ));
        CHECK(providerCalls == 0U);
    }

    TEST_CASE("a trusted query proving absence resolves and releases the target")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto firstRoot = toolRoot("proven-absent-delivery");
        auto firstCall = mutatingProjectCall(
            firstRoot,
            prepared.project,
            "proven-absent-delivery-run"
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto executor = ToolRuntimeExecutor{prepared.store};
        auto possible = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = firstRoot,
                .call       = firstCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            // Stated rather than converted, for the reason the terminally
            // unresolved case gives: this call is composed, so nothing turns
            // its failure into uncertainty and only the handler itself can
            // report inheriting a child's.
            [](ToolCallPositionIdentity const&) -> Result<ToolCallCompletion>
            {
                auto explanation = CanonicalJson::parseExact(
                    R"({"reason":"a child delivery never resolved"})"
                );
                REQUIRE(explanation.has_value());
                return ToolCallCompletion::possible(*explanation);
            }
        );
        auto const possibleWhy = possible.has_value()
            ? std::string{}
            : possible.error().message();
        REQUIRE_MESSAGE(possible.has_value(), possibleWhy);
        CHECK(possible->state == ToolCallState::Possible);

        auto explanation = CanonicalJson::parseExact(
            R"({"reason":"the target never received the input"})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"snapshot_ref":"unchanged-target-snapshot"})"
        );
        REQUIRE(explanation.has_value());
        REQUIRE(evidence.has_value());
        auto resolved = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [&explanation, &evidence](ToolCallPositionIdentity const&)
            {
                return ToolCallReconciliation::provenAbsent(
                    *explanation,
                    *evidence
                );
            }
        );
        auto const resolvedWhy = resolved.has_value()
            ? std::string{}
            : resolved.error().message();
        REQUIRE_MESSAGE(resolved.has_value(), resolvedWhy);
        CHECK(resolved->state == ToolCallState::ProvenAbsent);
        REQUIRE(resolved->evidence.has_value());
        CHECK(resolved->evidence->bytes() == evidence->bytes());

        // Proven absence is a resolution, so the target-wide barrier the
        // uncertainty raised is gone and a new mutating root is admitted.
        auto secondRoot = toolRoot("mutation-after-proven-absence");
        auto secondCall = mutatingProjectCall(
            secondRoot,
            prepared.project,
            "mutation-after-proven-absence-run"
        );
        auto delivered = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = secondRoot,
                .call       = secondCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [](ToolCallPositionIdentity const&)
            {
                auto result = CanonicalJson::parseExact(R"({"delivered":true})");
                REQUIRE(result.has_value());
                return ToolCallCompletion::confirmed(*result);
            }
        );
        auto const deliveredWhy = delivered.has_value()
            ? std::string{}
            : delivered.error().message();
        REQUIRE_MESSAGE(delivered.has_value(), deliveredWhy);
        CHECK(delivered->state == ToolCallState::Confirmed);
    }

    TEST_CASE("a mutating provider may prove absence without ever being uncertain")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto root      = toolRoot("provider-proven-absent");
        auto call      = mutatingProjectCall(
            root,
            prepared.project,
            "provider-proven-absent-run"
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto explanation = CanonicalJson::parseExact(
            R"({"reason":"the transport refused before the target saw anything"})"
        );
        auto evidence = CanonicalJson::parseExact(
            R"({"transport":"refused-before-send"})"
        );
        REQUIRE(explanation.has_value());
        REQUIRE(evidence.has_value());
        auto executor = ToolRuntimeExecutor{prepared.store};
        auto absent   = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = root,
                .call       = call,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [&explanation, &evidence](ToolCallPositionIdentity const&)
            {
                return ToolCallCompletion::provenAbsent(*explanation, *evidence);
            }
        );
        auto const absentWhy = absent.has_value()
            ? std::string{}
            : absent.error().message();
        REQUIRE_MESSAGE(absent.has_value(), absentWhy);
        CHECK(absent->state == ToolCallState::ProvenAbsent);
        REQUIRE(absent->evidence.has_value());
        CHECK(absent->evidence->bytes() == evidence->bytes());

        // Nothing is frozen: a provider that proved its effect never landed
        // left no uncertainty behind.
        auto secondRoot = toolRoot("mutation-after-provider-absence");
        auto secondCall = mutatingProjectCall(
            secondRoot,
            prepared.project,
            "mutation-after-provider-absence-run"
        );
        auto delivered = executor.invoke(
            ToolAdmissionRequest{
                .controller = prepared.controller,
                .lease      = prepared.lease,
                .root       = secondRoot,
                .call       = secondCall,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .planAuthority = prepared.planAuthority,
                    .effects       = effects,
                },
            },
            [](ToolCallPositionIdentity const&)
            {
                auto result = CanonicalJson::parseExact(R"({"delivered":true})");
                REQUIRE(result.has_value());
                return ToolCallCompletion::confirmed(*result);
            }
        );
        REQUIRE(delivered.has_value());
        CHECK(delivered->state == ToolCallState::Confirmed);
    }

    TEST_CASE("a read-only Tool cannot report a delivery classification")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto explanation = CanonicalJson::parseExact(
            R"({"reason":"the capture may or may not have happened"})"
        );
        auto evidence = CanonicalJson::parseExact(R"({"frame":"unknown"})");
        REQUIRE(explanation.has_value());
        REQUIRE(evidence.has_value());
        auto executor = ToolRuntimeExecutor{prepared.store};

        SUBCASE("possible is refused")
        {
            auto root = toolRoot("read-only-possible");
            auto call = frameworkCall(root, "read-only-possible-run");
            auto refused = executor.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                },
                [&explanation](ToolCallPositionIdentity const&)
                {
                    return ToolCallCompletion::possible(*explanation);
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "A read-only Tool cannot report possible"
            ));
        }

        SUBCASE("proven absence is refused")
        {
            auto root = toolRoot("read-only-proven-absent");
            auto call = frameworkCall(root, "read-only-proven-absent-run");
            auto refused = executor.invoke(
                ToolAdmissionRequest{
                    .controller = prepared.controller,
                    .lease      = prepared.lease,
                    .root       = root,
                    .call       = call,
                },
                [&explanation, &evidence](ToolCallPositionIdentity const&)
                {
                    return ToolCallCompletion::provenAbsent(
                        *explanation,
                        *evidence
                    );
                }
            );
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().message().contains(
                "A read-only Tool cannot report proven_absent"
            ));
        }
    }
}
