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
}
