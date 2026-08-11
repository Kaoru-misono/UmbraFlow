#pragma once

#include <task/host-delivery.hpp>
#include <task/page-model-file.hpp>
#include <task/task-context.hpp>
#include <task/task-host.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>

#include <script/engine.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// The one harness that reaches TaskHost's private delivery surface.
//
// TaskHost declares `friend struct TaskHostTestAccess` once, so uf::task holds
// exactly one type of that name and it is defined here rather than beside each
// caller: two definitions would be two spellings of the same privilege, and a
// program that saw both would be ill-formed. Both the Runtime v2 fixture under
// tests/support/ and the Operator's delivery fixture include this file.
//
// It is deliberately NOT a friend of HostDeliveryReport. It can reach TaskHost's
// privates and therefore call deliver, but a harness able to fabricate what
// deliver returns would make every test over that value unfalsifiable, and
// friendship is not transitive.
//
// It names nothing from operator/, so a test binary that links only task can
// include it.
namespace uf::task
{
    // The one UI action a harness drives, in the vocabulary of the RuntimeModel
    // it was handed. It is a value rather than three parameters because the
    // chunk that mints a Receipt and the check that reads one must name the same
    // action, and a delivering Host holds it for the life of a dispatch.
    struct UiActionUnderTest final
    {
        std::string surface{};
        std::string uiTarget{};
        std::string action{};
    };

    // RM identifiers, as the trusted model parser defines them. Checked here
    // because the two below are interpolated into a Luau chunk: a model comes
    // from the project under test, and an identifier that could carry a quote
    // would let a supplied model write the chunk that drives it.
    [[nodiscard]]
    inline auto isRuntimeModelIdentifier(std::string_view value) -> bool
    {
        auto const tail = [](char character)
        {
            return (character >= 'a' && character <= 'z')
                || (character >= '0' && character <= '9')
                || character == '.'
                || character == '_'
                || character == ':'
                || character == '-';
        };
        return !value.empty()
            && value.front() >= 'a'
            && value.front() <= 'z'
            && std::ranges::all_of(value, tail);
    }

    // The trusted chunk that asks the Host for one click authorization against
    // `action`. It is the resolver's own vocabulary: nothing here names a
    // coordinate, and the Receipt it mints is Host-private storage the chunk
    // never sees. The surface is absent because the chunk does not choose one --
    // resolve_state does, out of the model -- and checking the surface it
    // reached is pendingReceipt's job.
    [[nodiscard]]
    inline auto authorizeClickSource(UiActionUnderTest const& action) -> std::string
    {
        REQUIRE(isRuntimeModelIdentifier(action.uiTarget));
        REQUIRE(isRuntimeModelIdentifier(action.action));
        return std::format(
            R"lua(
        local cycle = observe.open(project.load_project())
        local state = cycle:resolve_state()
        local binding = cycle:resolve_binding(state, "{}")
        local receipt, reason = cycle:authorize(binding, "{}")
        if receipt == nil then error(reason) end
        return 1
    )lua",
            action.uiTarget,
            action.action
        );
    }

    struct TaskHostTestAccess final
    {
        [[nodiscard]]
        static auto activate(
            TaskHost& host,
            std::filesystem::path const& artifactRoot,
            ContentHash const& expectedRootHash
        ) -> Result<GenerationId>
        {
            UF_TRY_VALUE(
                verified,
                uf::task::loadRuntimeArtifact(artifactRoot, expectedRootHash)
            );
            auto artifact = std::make_shared<RuntimeArtifactHandle const>(
                std::move(verified)
            );
            return host.activateRuntimeArtifact(
                InstalledRuntimeArtifact{std::move(artifact), 1U}
            );
        }

        [[nodiscard]]
        static auto run(
            TaskHost& host,
            GenerationId generation,
            TaskContext& context,
            std::string_view source
        ) -> Result<script::ScriptValue>
        {
            return host.runTrustedRuntime(
                generation,
                context,
                source,
                "host-delivery-fixture"
            );
        }

        // The one unconsumed Receipt, and the check that the Host minted it for
        // the action the caller asked for. The surface is checked here because
        // it is the one field the chunk cannot state: a model whose frame
        // resolves to another scene, or to none, reaches this and goes red.
        //
        // The binding, variant and proof locator are checked for presence only.
        // They are the model's own answer to the surface/target/action triple,
        // so a caller restating them would be restating the model rather than
        // testing it.
        [[nodiscard]]
        static auto pendingReceipt(
            TaskHost& host,
            UiActionUnderTest const& expected
        ) -> TaskHost::Receipt
        {
            REQUIRE(host.m_receipts.size() == 1U);
            auto const& pending = host.m_receipts.front();
            CHECK(pending.intent.stateIdentity.starts_with("state-resolution-"));
            CHECK(pending.intent.surface == expected.surface);
            CHECK(pending.intent.uiTarget == expected.uiTarget);
            CHECK(pending.intent.action == expected.action);
            CHECK_FALSE(pending.intent.binding.empty());
            CHECK_FALSE(pending.intent.variant.empty());
            CHECK_FALSE(pending.intent.proofLocator.empty());
            return TaskHost::Receipt{host.m_hostNonce, pending.ordinal};
        }

        [[nodiscard]] static auto pendingSemanticHash(TaskHost const& host) -> ContentHash
        {
            REQUIRE(host.m_receipts.size() == 1U);
            return host.m_receipts.front().semanticHash;
        }

        // The authority is the caller's, never the Host's: a harness that could
        // ask the Host what to present would prove only that the Host agrees
        // with itself.
        [[nodiscard]]
        static auto deliver(
            TaskHost& host,
            DispatchAuthority authority,
            TaskHost::Receipt const& receipt,
            TaskContext& context
        ) -> Result<HostDeliveryReport>
        {
            return host.deliver(std::move(authority), receipt, context);
        }

        [[nodiscard]]
        static auto adoptControlFence(TaskHost& host, ControlFence fence) -> Status
        {
            return host.adoptControlFence(std::move(fence));
        }

        [[nodiscard]] static auto fence(TaskHost const& host) -> ControlFence
        {
            return host.m_fence;
        }
    };
}
