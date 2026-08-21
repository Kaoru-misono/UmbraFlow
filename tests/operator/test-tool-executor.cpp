#include <operator/tool-executor.hpp>

#include "project-fixture.hpp"

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
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
            auto call = ToolCallPositionIdentity::create(
                root,
                std::nullopt,
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
            auto invocation = test_support::toolInvocation(project, "command-1");
            REQUIRE(invocation.descriptor().mutability == ToolMutability::Mutating);
            auto call = ToolCallPositionIdentity::create(
                root,
                std::nullopt,
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
            auto replay = executor.invokeReadOnly(
                prepared.controller,
                prepared.lease,
                *root,
                call,
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
            auto failed = executor.invokeReadOnly(
                prepared.controller,
                prepared.lease,
                *failureRoot,
                failureCall,
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
            auto replayedFailure = executor.invokeReadOnly(
                prepared.controller,
                prepared.lease,
                *failureRoot,
                failureCall,
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
        auto replay = executor.invokeReadOnly(
            prepared.controller,
            prepared.lease,
            *root,
            call,
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
        auto refused = executor.invokeReadOnly(
            prepared.controller,
            prepared.lease,
            *missingRoot,
            missingCall,
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
        auto executor      = ToolRuntimeExecutor{prepared.store};
        auto providerCalls = uint64{};
        auto possible = executor.invokeMutating(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [&providerCalls](ToolCallPositionIdentity const&)
                -> Result<ToolCallCompletion>
            {
                ++providerCalls;
                return fail(
                    AutomationErrorKind::IoFailure,
                    "input transport did not prove delivery"
                );
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
        auto blocked = executor.invokeMutating(
            prepared.controller,
            prepared.lease,
            secondRoot,
            secondCall,
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
            test_support::toolInvocation(prepared.project, "command-1")
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
        auto observed = executor.invokeReadOnly(
            prepared.controller,
            prepared.lease,
            observeRoot,
            observeCall,
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
        auto reconciliation = ToolCallReconciliation::confirmed(
            *reconciledResult,
            *evidence
        );
        auto reconciled = prepared.store.reconcileMutatingToolCall(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            reconciliation
        );
        REQUIRE(reconciled.has_value());
        REQUIRE(reconciled->evidence.has_value());
        CHECK(reconciled->state == ToolCallState::Confirmed);
        CHECK(reconciled->evidence->bytes() == evidence->bytes());

        auto unblocked = executor.invokeMutating(
            prepared.controller,
            prepared.lease,
            secondRoot,
            secondCall,
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
        auto executor = ToolRuntimeExecutor{prepared.store};
        auto error = CanonicalJson::parseExact(R"({"delivery":"unknown"})");
        REQUIRE(error.has_value());
        auto possible = executor.invokeMutating(
            prepared.controller,
            prepared.lease,
            firstRoot,
            firstCall,
            [&error](ToolCallPositionIdentity const&)
            {
                return ToolCallCompletion::terminalFailure(*error);
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
            ToolCallReconciliation::terminallyUnresolved(
                *explanation,
                *evidence
            )
        );
        REQUIRE(unresolved.has_value());
        CHECK(unresolved->state == ToolCallState::TerminallyUnresolved);

        auto secondRoot = toolRoot("blocked-after-terminally-unresolved");
        auto secondCall = mutatingProjectCall(
            secondRoot,
            prepared.project,
            "blocked-after-terminally-unresolved-run"
        );
        auto blocked = executor.invokeMutating(
            prepared.controller,
            prepared.lease,
            secondRoot,
            secondCall,
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
            "command-1"
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
        auto providerCalls = uint64{};
        auto executor      = ToolRuntimeExecutor{prepared.store};
        auto blocked = executor.invokeMutating(
            prepared.controller,
            prepared.lease,
            root,
            call,
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
}
