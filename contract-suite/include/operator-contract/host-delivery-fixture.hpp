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

#include <filesystem>
#include <memory>
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
    // The trusted chunk that asks the Host for one click authorization. It is
    // the resolver's own vocabulary: nothing here names a coordinate, and the
    // Receipt it mints is Host-private storage the chunk never sees. Both
    // fixture RuntimeModels declare the confirm.primary/activate binding it
    // resolves.
    inline constexpr auto k_authorizeClickSource = std::string_view{R"lua(
        local cycle = observe.open(project.load_project())
        local state = cycle:resolve_state()
        local binding = cycle:resolve_binding(state, "confirm")
        local receipt, reason = cycle:authorize(binding, "activate")
        if receipt == nil then error(reason) end
        return 1
    )lua"};

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

        // The one unconsumed Receipt, with the intent both fixture RuntimeModels
        // describe. Every model this harness serves declares the same
        // confirm.primary/activate binding, so the assertions below hold for all
        // of them and go red when a model stops declaring it.
        [[nodiscard]] static auto pendingReceipt(TaskHost& host) -> TaskHost::Receipt
        {
            REQUIRE(host.m_receipts.size() == 1U);
            auto const& pending = host.m_receipts.front();
            CHECK(pending.intent.stateIdentity.starts_with("state-resolution-"));
            CHECK(pending.intent.surface == "screen");
            CHECK(pending.intent.uiTarget == "confirm");
            CHECK(pending.intent.binding == "confirm.primary");
            CHECK(pending.intent.variant == "primary");
            CHECK(pending.intent.action == "activate");
            CHECK(pending.intent.proofLocator == "confirm-mark");
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
