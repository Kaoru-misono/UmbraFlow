#pragma once

#include "project-fixture.hpp"

#include <operator/ledger.hpp>
#include <operator/tool-admission-request.hpp>
#include <operator/tool-invocation.hpp>
#include <operator/tool-runtime.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime::test_support
{
    // A position at a chosen ordinal, reached through the runtime's issuer.
    [[nodiscard]]
    inline auto toolCallAt(
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const* parent,
        uint32 ordinal,
        ToolExecutionIdentity const& executionIdentity,
        ValidatedToolInvocation const& invocation
    ) -> Result<ToolCallPositionIdentity>
    {
        REQUIRE(ordinal > 0U);
        auto context = parent != nullptr
            ? ToolCallIssuingContext::forHandler(*parent)
            : ToolCallIssuingContext::forRoot(root, executionIdentity);
        auto position = context.issue(invocation);
        for (auto index = uint32{1}; index < ordinal; ++index)
        {
            position = context.issue(invocation);
        }
        return position;
    }

    // How far a fixture drives one call. Admitted stops at the durable
    // admission row; Dispatching crosses the durable dispatch boundary, which
    // is the state the target-wide mutation barrier and the release-upgrade
    // quiescence gate both observe.
    enum class ToolCallReach : uint8
    {
        Admitted,
        Dispatching,
    };

    // One Tool call in flight on this store, and the coordinates a case needs
    // to name it again.
    //
    // It is move-only because ToolCallAdmission and ToolCallDispatch are: each
    // is a one-shot right to cross a durable boundary, and a value two holders
    // each believe they hold is two rights.
    struct StartedToolCall final
    {
        ToolRootRequestIdentity         root;
        ToolCallPositionIdentity        call;
        ToolCallAdmission               admission;
        std::optional<ToolCallDispatch> dispatch{};
    };

    // Puts one call in flight under the binding and lease a case names, the
    // only way production does: a root request, a positioned call under it, an
    // admission re-read against live authority, and -- for Dispatching -- the
    // durable dispatch boundary.
    //
    // Mutability is not a parameter. It is read from the Tool Catalog
    // descriptor the invocation carries, exactly as admitToolCall reads it, so
    // a case cannot present a mutating tool as read-only here either; the
    // effect envelope is attached when and only when the descriptor says the
    // tool mutates.
    //
    // controller and lease are call-scoped borrows of the caller's own values
    // and nothing here retains them.
    [[nodiscard]]
    inline auto startToolCall(
        PreparedStore& prepared,
        ControllerBinding const& controller,
        ControlLease const& lease,
        std::string_view requestKey,
        std::string_view toolName,
        ToolCallReach reach = ToolCallReach::Dispatching
    ) -> StartedToolCall
    {
        auto preimage = CanonicalJson::parseExact(
            R"({"objective":"fixture-tool-call"})"
        );
        REQUIRE(preimage.has_value());
        auto root = ToolRootRequestIdentity::create(
            std::string{controller.controllerId()},
            std::string{requestKey},
            *std::move(preimage)
        );
        REQUIRE(root.has_value());

        auto const invocation = toolInvocation(
            prepared.project,
            std::string{toolName}
        );
        auto const execution = ToolExecutionIdentity{
            .runIdentity                 = prepared.manifest.hash(),
            .frameworkReleaseIdentity    = prepared.runtimeArtifactRootHash,
            .toolRuntimeProtocolIdentity = hashOf("fixture-tool-protocol"),
            .environmentIdentity         = hashOf("fixture-tool-environment"),
        };
        auto call = toolCallAt(*root, nullptr, 1U, execution, invocation);
        REQUIRE(call.has_value());

        auto mutation = std::optional<ToolAdmissionRequest::Mutation>{};
        if (invocation.descriptor().mutability == ToolMutability::Mutating)
        {
            mutation = ToolAdmissionRequest::Mutation{
                .effects = {
                    routineToolEffect(prepared.project, std::string{toolName}),
                },
            };
        }

        auto admission = prepared.store.admitToolCall(ToolAdmissionRequest{
            .controller      = controller,
            .lease           = lease,
            .root            = *root,
            .call            = *call,
            .policyAuthority = prepared.policyAuthority,
            .mutation        = std::move(mutation),
        });
        if (!admission.has_value())
        {
            FAIL(admission.error().message());
        }

        auto started = StartedToolCall{
            .root      = *std::move(root),
            .call      = *std::move(call),
            .admission = *std::move(admission),
        };
        if (reach == ToolCallReach::Dispatching)
        {
            auto dispatch = prepared.store.beginToolCallDispatch(started.admission);
            REQUIRE(dispatch.has_value());
            started.dispatch = *std::move(dispatch);
        }
        return started;
    }

    // The same call started under the store's own pinned binding and lease,
    // which is what the great majority of cases want.
    [[nodiscard]]
    inline auto startToolCall(
        PreparedStore& prepared,
        std::string_view requestKey,
        std::string_view toolName,
        ToolCallReach reach = ToolCallReach::Dispatching
    ) -> StartedToolCall
    {
        return startToolCall(
            prepared,
            prepared.controller,
            prepared.lease,
            requestKey,
            toolName,
            reach
        );
    }

    // Takes a dispatching call to its settled terminal, which is what releases
    // the mutation barrier it holds.
    inline auto confirmToolCall(
        PreparedStore& prepared,
        StartedToolCall const& started
    ) -> void
    {
        REQUIRE(started.dispatch.has_value());
        auto payload = CanonicalJson::parseExact(R"({"done":true})");
        REQUIRE(payload.has_value());
        auto const outcome = prepared.store.completeToolCallDispatch(
            *started.dispatch,
            ToolCallCompletion::confirmed(*std::move(payload))
        );
        if (!outcome.has_value())
        {
            FAIL(outcome.error().message());
        }
        CHECK(outcome->state == ToolCallState::Confirmed);
    }
}
