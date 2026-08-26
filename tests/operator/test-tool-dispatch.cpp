#include <operator/project-tool-dispatch.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include <json/value.hpp>

#include "project-fixture.hpp"

#include <doctest/doctest.h>

#include <stop_token>
#include <string>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        using test_support::TemporaryDirectory;
        using test_support::hashOf;

        template <typename Value>
        [[nodiscard]] auto why(Result<Value> const& value) -> std::string
        {
            return value.has_value()
                ? std::string{}
                : std::string{value.error().message()};
        }

        [[nodiscard]] auto replayPayload(ToolCallReplay const& replay) -> std::string
        {
            return replay.payload ? replay.payload->bytes() : std::string{};
        }

        struct LeafCall final
        {
            ToolRootRequestIdentity  root;
            ToolCallPositionIdentity call;
        };

        [[nodiscard]]
        auto leafCall(
            test_support::PreparedStore const& prepared,
            std::string_view requestKey
        ) -> LeafCall
        {
            auto preimage = CanonicalJson::parseExact(
                R"({"objective":"dispatch one Project Tool leaf"})"
            );
            REQUIRE(preimage.has_value());
            auto root = ToolRootRequestIdentity::create(
                std::string{prepared.controller.controllerId()},
                std::string{requestKey},
                *std::move(preimage)
            );
            REQUIRE(root.has_value());

            auto invocation = test_support::toolInvocation(
                prepared.project,
                prepared.project.toolName("observe-1")
            );
            auto context = ToolCallIssuingContext::forRoot(
                *root,
                ToolExecutionIdentity{
                    .runIdentity                 = prepared.manifest.hash(),
                    .frameworkReleaseIdentity    = prepared.runtimeArtifactRootHash,
                    .toolRuntimeProtocolIdentity = hashOf("fixture-tool-protocol"),
                    .environmentIdentity         = hashOf("fixture-tool-environment"),
                }
            );
            auto call = context.issue(invocation);
            REQUIRE_MESSAGE(call.has_value(), why(call));
            return LeafCall{
                .root = *std::move(root),
                .call = *std::move(call),
            };
        }

        [[nodiscard]]
        auto requestFor(
            test_support::PreparedStore const& prepared,
            LeafCall const& positioned
        ) -> ToolAdmissionRequest
        {
            return ToolAdmissionRequest{
                .controller      = prepared.controller,
                .lease           = prepared.lease,
                .root            = positioned.root,
                .call            = positioned.call,
                .policyAuthority = prepared.policyAuthority,
            };
        }
    }

    TEST_CASE("a dispatched Project Tool leaf runs its bound handler and records its answer")
    {
        auto temporary  = TemporaryDirectory{};
        auto prepared   = test_support::prepareStore(temporary.path());
        auto dispatcher = ProjectToolDispatcher::create(prepared.store);
        REQUIRE_MESSAGE(dispatcher.has_value(), why(dispatcher));
        auto const positioned = leafCall(prepared, "project-leaf-dispatch");
        auto const request    = requestFor(prepared, positioned);

        auto const answered = dispatcher->dispatch(
            prepared.generation,
            request,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(answered.has_value(), why(answered));
        CHECK(answered->state == ToolCallState::Confirmed);
        CHECK(replayPayload(*answered) == R"({"entry":"observe-1"})");

        auto const replayed = dispatcher->dispatch(
            prepared.generation,
            request,
            std::stop_token{}
        );
        REQUIRE_MESSAGE(replayed.has_value(), why(replayed));
        CHECK(replayed->state == ToolCallState::Confirmed);
        CHECK(replayPayload(*replayed) == replayPayload(*answered));
    }
}
