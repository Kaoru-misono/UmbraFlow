#include <operator/tool-executor.hpp>

#include "project-fixture.hpp"
#include "tool-call-fixture.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
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
                "framework.screen.capture",
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
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *root,
                    .call            = call,
                    .policyAuthority = prepared.policyAuthority,
                },
                [&providerCalls, &result](ToolCallPositionIdentity const& presented)
                {
                    ++providerCalls;
                    CHECK(presented.toolName() == "framework.screen.capture");
                    return ToolCallCompletion::confirmed(*result);
                }
            );
            REQUIRE(replay.has_value());
            REQUIRE(replay->payload.has_value());
            CHECK(replay->state == ToolCallState::Confirmed);
            CHECK(replay->payload->bytes() == result->bytes());
            CHECK(providerCalls == 1U);
            auto const confirmedAnswer = toolCallAnswer(call.identity(), *replay);
            REQUIRE(confirmedAnswer.has_value());
            auto const expectedConfirmed = R"({"call_identity":")"
                + call.identity().hex()
                + R"(","delivery":"confirmed","ok":true,"result":{"snapshot_ref":"snapshot-1"}})";
            CHECK(
                json::canonicalBytes(*confirmedAnswer)
                == expectedConfirmed
            );

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
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *failureRoot,
                    .call            = failureCall,
                    .policyAuthority = prepared.policyAuthority,
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
                == R"({"code":"capture_unavailable","message":"capture provider refused","retryable":false})"
            );
            auto const failedAnswer = toolCallAnswer(
                failureCall.identity(),
                *failed
            );
            REQUIRE(failedAnswer.has_value());
            auto const expectedFailure = R"({"call_identity":")"
                + failureCall.identity().hex()
                + R"(","delivery":"terminal_failure","error":{"code":"capture_unavailable","message":"capture provider refused","retryable":false},"ok":false})";
            CHECK_MESSAGE(
                json::canonicalBytes(*failedAnswer) == expectedFailure,
                "provider failure must reach the direct answer with its own message verbatim"
            );
            auto refusedReplayExecutions = uint64{};
            auto replayedFailure = executor.invoke(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = *failureRoot,
                    .call            = failureCall,
                    .policyAuthority = prepared.policyAuthority,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = *missingRoot,
                .call            = missingCall,
                .policyAuthority = prepared.policyAuthority,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = firstRoot,
                .call            = firstCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = secondRoot,
                .call            = secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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

        // The barrier belongs to the ledger and not to the executor wrapper, so
        // the store's own admission door refuses the same call the executor
        // just refused. A guard that lived in the executor would let anything
        // reaching admitToolCall directly straight past a frozen target.
        auto directRoot = toolRoot("mutating-blocked-direct");
        auto directCall = mutatingProjectCall(
            directRoot,
            prepared.project,
            "mutating-blocked-direct-run"
        );
        auto directlyBlocked = prepared.store.admitToolCall(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = directRoot,
                .call            = directCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
                },
            }
        );
        REQUIRE_FALSE(directlyBlocked.has_value());
        CHECK(directlyBlocked.error().message().contains(
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = observeRoot,
                .call            = observeCall,
                .policyAuthority = prepared.policyAuthority,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = secondRoot,
                .call            = secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
            prepared.policyAuthority,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects   = effects,
                    .approvals = approvals,
                },
            },
            provider
        );
        REQUIRE(invoked.has_value());
        CHECK(invoked->state == ToolCallState::Confirmed);
        CHECK(providerCalls == 1U);

        auto replayed = executor.invoke(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = firstRoot,
                .call            = firstCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = secondRoot,
                .call            = secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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

    // Any unterminated mutating call holds the barrier, not only one that has
    // become uncertain: a second mutation admitted beside a dispatching one
    // would be two mutations aimed at one target with no order between them.
    TEST_CASE("a dispatching mutation and a second Tool mutation cannot run beside each other")
    {
        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const holding = test_support::startToolCall(
            prepared,
            "dispatching-mutation",
            prepared.project.toolName("command-1")
        );
        REQUIRE(holding.dispatch.has_value());

        auto root = toolRoot("tool-beside-dispatching-mutation");
        auto call = mutatingProjectCall(
            root,
            prepared.project,
            "tool-beside-dispatching-mutation-run"
        );
        auto effects = std::vector{
            test_support::routineToolEffect(prepared.project),
        };
        auto providerCalls = uint64{};
        auto executor      = ToolRuntimeExecutor{prepared.store};
        auto blocked = executor.invoke(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
        CHECK(blocked.error().message().contains("frozen by Tool call"));
        CHECK(blocked.error().message().contains(
            holding.call.identity().hex()
        ));
        CHECK(providerCalls == 0U);

        // The barrier is released by settling the call that holds it, so the
        // same second call is admitted once the first reaches its terminal.
        test_support::confirmToolCall(prepared, holding);
        auto released = executor.invoke(
            ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
        if (!released.has_value())
        {
            FAIL(released.error().message());
        }
        CHECK(released->state == ToolCallState::Confirmed);
        CHECK(providerCalls == 1U);
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = firstRoot,
                .call            = firstCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = secondRoot,
                .call            = secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = root,
                .call            = call,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = secondRoot,
                .call            = secondCall,
                .policyAuthority = prepared.policyAuthority,
                .mutation   = ToolAdmissionRequest::Mutation{
                    .effects         = effects,
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
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = root,
                    .call            = call,
                    .policyAuthority = prepared.policyAuthority,
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
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = root,
                    .call            = call,
                    .policyAuthority = prepared.policyAuthority,
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

    // The ceiling a Project declared, enforced. Both arms share one prepared
    // store because the store IS the cost here: what separates them is one
    // enumerator in one descriptor, and running that difference against two
    // stores would buy nothing.
    TEST_CASE(
        "a Tool call that outran its declared ceiling reports the ceiling and "
        "its on_timeout decides the run"
    )
    {
        constexpr auto k_declaredCeilingMillis = uint64{1U};
        constexpr auto k_lateProviderWork      = std::chrono::milliseconds{25};

        auto temporary = test_support::TemporaryDirectory{};
        auto prepared  = test_support::prepareStore(temporary.path());
        auto const toolName = prepared.project.toolName("observe-1");

        // Descriptors this suite states over the registration's OWN declaration
        // bytes. The catalog identity is the hash of those bytes, so a trusted
        // reader may shrink a ceiling to one a case can cross in milliseconds
        // without moving anything the session pinned -- which is the same seam
        // a deployment reader occupies.
        auto const catalogWith = [&prepared, &toolName](TimeoutAction action)
        {
            auto descriptor = test_support::toolInvocation(
                prepared.project,
                toolName
            ).descriptor();
            REQUIRE(descriptor.mutability == ToolMutability::ReadOnly);
            descriptor.timeout = TimeoutPolicy{
                .maximumElapsedMillis = k_declaredCeilingMillis,
                .onTimeout            = action,
            };
            auto tools = std::vector<ToolCatalogEntry>{
                ToolCatalogEntry{
                    .name        = toolName,
                    .description = "A fixture Project Tool leaf.",
                    .inputSchema = json::Value::ofObject({}),
                    .descriptor  = std::move(descriptor),
                },
            };
            auto owner = ProjectToolCatalogSchemaOwner::create(
                prepared.project.registration,
                prepared.project.toolCatalogBytes,
                [tools = std::move(tools)]()
                    -> Result<std::vector<ToolCatalogEntry>> { return tools; },
                [](std::string_view, std::string_view) -> Status { return ok(); }
            );
            REQUIRE(owner.has_value());
            return *std::move(owner);
        };

        auto const callUnder = [&toolName](
            ProjectToolCatalogSchemaOwner const& catalog,
            ToolRootRequestIdentity const& root,
            std::string_view runName
        )
        {
            auto arguments = CanonicalJson::parseExact(R"({"value":1})");
            REQUIRE(arguments.has_value());
            auto invocation = catalog.validate(toolName, *std::move(arguments));
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
        };

        // A provider that answers correctly and late. It answers at all because
        // the overrun is what decides the outcome: an answer that arrived past
        // the ceiling is not the answer the Project declared it would take.
        auto const lateProvider = [k_lateProviderWork](
            ToolCallPositionIdentity const&
        ) -> Result<ToolCallCompletion>
        {
            std::this_thread::sleep_for(k_lateProviderWork);
            auto answer = CanonicalJson::parseExact(R"({"observed":true})");
            REQUIRE(answer.has_value());
            return ToolCallCompletion::confirmed(*std::move(answer));
        };

        auto executor = ToolRuntimeExecutor{prepared.store};

        {
            auto const catalog = catalogWith(TimeoutAction::Stop);
            auto const root    = toolRoot("executor-timeout-stop");
            auto const call    = callUnder(
                catalog,
                root,
                "executor-timeout-stop-run"
            );
            auto stopped = executor.invoke(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = root,
                    .call            = call,
                    .policyAuthority = prepared.policyAuthority,
                },
                lateProvider
            );

            // `stop` is a returned FAILURE, which is what ends a run at every
            // caller adapter and at the scoped seam alike.
            REQUIRE_FALSE(stopped.has_value());
            CHECK(
                automationErrorKind(stopped.error())
                == AutomationErrorKind::Timeout
            );

            // The signal names WHAT was exceeded and WHAT the limit was. A
            // generic failure here would be a limit the Project declared and
            // then could not act on.
            CHECK(stopped.error().message().contains(toolName));
            CHECK(stopped.error().message().contains(
                "exceeded the maximum_elapsed_ms of 1 its Tool declared"
            ));

            // The overrun is DURABLE and not merely reported: replaying the
            // coordinate answers out of the row without running anything.
            auto executions = uint64{};
            auto replayed   = executor.invoke(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = root,
                    .call            = call,
                    .policyAuthority = prepared.policyAuthority,
                },
                [&executions](ToolCallPositionIdentity const&)
                {
                    ++executions;
                    auto answer = CanonicalJson::parseExact("{}");
                    REQUIRE(answer.has_value());
                    return ToolCallCompletion::confirmed(*std::move(answer));
                }
            );
            REQUIRE(replayed.has_value());
            REQUIRE(replayed->payload.has_value());
            CHECK(executions == 0U);
            CHECK(replayed->state == ToolCallState::TerminalFailure);
            auto const& payload = replayed->payload->bytes();
            CHECK(payload.find(R"("code":"timeout")") != std::string::npos);
            CHECK(payload.find("exceeded the maximum_elapsed_ms of 1")
                  != std::string::npos);
            CHECK(payload.find(R"("retryable":false)") != std::string::npos);
        }

        {
            auto const catalog = catalogWith(TimeoutAction::Reobserve);
            auto const root    = toolRoot("executor-timeout-reobserve");
            auto const call    = callUnder(
                catalog,
                root,
                "executor-timeout-reobserve-run"
            );
            auto reobserved = executor.invoke(
                ToolAdmissionRequest{
                    .controller      = prepared.controller,
                    .lease           = prepared.lease,
                    .root            = root,
                    .call            = call,
                    .policyAuthority = prepared.policyAuthority,
                },
                lateProvider
            );

            // `reobserve` is a returned VALUE: the run carries on holding the
            // recorded timeout, which is what lets the Project look again
            // rather than be torn down.
            REQUIRE(reobserved.has_value());
            REQUIRE(reobserved->payload.has_value());
            CHECK(reobserved->state == ToolCallState::TerminalFailure);
            auto const& payload = reobserved->payload->bytes();
            CHECK(payload.find(R"("code":"timeout")") != std::string::npos);
            CHECK(payload.find("exceeded the maximum_elapsed_ms of 1")
                  != std::string::npos);
            CHECK(payload.find(R"("retryable":false)") != std::string::npos);

            // What the provider answered is deliberately NOT the outcome.
            CHECK(payload.find(R"("observed")") == std::string::npos);
        }
    }
}
