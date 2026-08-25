#pragma once

#include <operator/controller.hpp>
#include <operator/host-controller.hpp>
#include <operator/ledger.hpp>
#include <operator/project-tool-dispatch.hpp>
#include <operator/tool-actor-adapters.hpp>
#include <operator/tool-invocation.hpp>

#include <task/task-context.hpp>
#include <task/task-host.hpp>
#include <task/runtime-model-file.hpp>
#include <task/ui-observation.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace uf::service
{
    // Whether this lifecycle may still ask for a mutating Tool. It starts
    // writable, because start() acquires the control lease before it returns,
    // and becomes read-only exactly when that lease is given up.
    enum class LifecycleAccess : uint8
    {
        ReadOnly,
        Writable,
    };

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ProductStart final
    {
        std::filesystem::path    projectDirectory{};
        std::filesystem::path    runtimeDirectory{};
        std::string              authenticatedControllerId{};
        std::vector<std::string> controllerCapabilities{};
        std::string              controlledTargetId{};

        // Which principal this session is pinned as, and deliberately without
        // a default: the kind is not a label on the controller id beside it,
        // it is the ceiling the whole Operator reads. An Agent reaches only
        // the semantic Tool surface; only a Human may approve a mutating Tool
        // or report external input about a third party. A default here would be
        // this module choosing a principal for a caller that did not state one,
        // which is exactly how one actor comes to hold another's powers.
        operator_runtime::ControllerKind kind;

        // The exact AgentProfile bytes this session is pinned to. Required for
        // every kind: every session spends the operator's machine, so every
        // session declares what it may spend, and a ceiling the operator chose
        // not to bind is the "unbounded" marker rather than a missing document.
        // They are bytes rather than a budget value because their hash IS the
        // manifest's agent_profile_hash: a caller that stated ceilings instead
        // would be naming a budget no manifest has to agree with.
        std::string agentProfileJcs{};

        // The observed-instance world this session observes in. It is
        // transferred into the session pin unchanged, so the observations this
        // lifecycle produces are bound to one scope.
        operator_runtime::ObservedInstanceWorldScope worldScope;
    };

    struct ProductIdentity final
    {
        std::filesystem::path     projectDirectory{};
        std::filesystem::path     runtimeArtifactRoot{};
        std::string               deployment{};
        std::string               pluginId{};
        ContentHash               registrationHash;
        task::RuntimeModelBinding runtimeModel;
        uint64                    installedGeneration{};

        // The two values that name this run inside the Operator: the session
        // row that start pinned, and the SessionManifest hash it was admitted
        // against. They are published because a stream a caller records beside
        // this lifecycle has to be joinable back to that row, and a caller that
        // minted its own name for the stream would name a session the ledger
        // has never heard of.
        std::string sessionId{};
        ContentHash sessionManifestHash;
    };

    struct ProductObservation final
    {
        operator_runtime::SnapshotRecord snapshot;
        task::UiObservationSnapshot      ui;
    };

    // The trusted adapter input for one top-level Framework Tool, read-only or
    // mutating. There is one request type and one seam for both, for the reason
    // ToolAdmissionRequest gives: the two differ in what the request carries --
    // a mutation proposal or none -- and never in which function was called.
    // The mutability itself is read off the descriptor the Framework Tool
    // Catalog holds, so an adapter cannot state one the catalog disagrees with.
    //
    // Caller namespace is deliberately absent: ProductLifecycle derives it
    // from the authenticated controller binding, so an adapter cannot attach a
    // request key to another principal's durable root. The remaining identity
    // hashes are fixed before dispatch by the adapter generation that invokes
    // this internal API; public actor envelopes will derive them rather than
    // accepting them from untrusted wire input.
    //
    // The call ordinal is deliberately absent too, and is not an omission a
    // later envelope may repair: per R4 the sequence is a monotone child index
    // assigned exclusively by the issuing seam, because a caller that could
    // name one could alias another call's position and inherit its recorded
    // outcome without authorisation. ProductLifecycle assigns it.
    //
    // executionIdentity describes the run, not the call. The first call under
    // one root request opens that root's issuing context and fixes it there,
    // and every later call under the same root is stamped with what the
    // context holds.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init)
    struct ToolRootCall final
    {
        std::string                             requestKey{};
        std::string                             exactRootRequestPreimageJcs{};
        operator_runtime::ToolExecutionIdentity executionIdentity;
        std::string                             toolName{};
        std::string                             exactArgumentsJcs{};

        // The structured body a descriptor-declared call holds its scope for.
        // It runs INSIDE this call's own dispatch, so the Tool calls it
        // makes are numbered under this call's durable position and recorded as
        // its children
        // (docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
        //
        // Move-only, because it is consumed exactly once: a body that could be
        // copied could be run twice under one recorded position.
        operator_runtime::ToolBodyRun body{};
    };

    // The production session over an Operator root. The exact published
    // Operator protocol schema has a production reader here: its bytes are
    // hashed into SessionManifest and satisfied again, byte for byte, by the
    // session's policy authority.
    //
    // This module holds the only production calls that open an
    // OperatorCoordinator -- start below, and reclaimRuntimeArtifacts,
    // upgradeRuntimeArtifactAndPinSession and approveReleaseCapabilities after
    // the class -- so every one reaches a root through
    // OperatorCoordinator::open and completes its recovery before doing
    // anything else. No production type outside this module constructs one.
    class ProductLifecycle final
    {
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        explicit ProductLifecycle(std::unique_ptr<Impl> implementation);

        // The Framework provider surface lives on Impl rather than here. It is
        // installed into a ProjectToolDispatcher that outlives every call, and
        // a provider bound to this object would be bound to an address a move
        // of this handle invalidates; Impl is heap-allocated, non-copyable and
        // non-movable, so a pointer to it is a lifetime contract the dispatcher
        // can be held to. See product-lifecycle.cpp.

    public:
        ProductLifecycle(ProductLifecycle&&) noexcept;
        auto operator=(ProductLifecycle&&) noexcept -> ProductLifecycle& = delete;
        ProductLifecycle(ProductLifecycle const&) = delete;
        auto operator=(ProductLifecycle const&) -> ProductLifecycle& = delete;
        ~ProductLifecycle();

        [[nodiscard]]
        static auto start(ProductStart const& start) -> Result<ProductLifecycle>;

        [[nodiscard]] auto access() const noexcept -> LifecycleAccess;

        [[nodiscard]] auto identity() const -> ProductIdentity;


        [[nodiscard]]
        auto observe(task::TaskContext& context)
            -> Result<ProductObservation>;

        // Opens this session's exploration front end over `config`'s live
        // ports. There is ONE production door onto a desktop, and this is
        // annotation's side of it: the session that comes back records into the
        // trace stream this lifecycle's Operator session names, observes
        // through the RuntimeModel this lifecycle pinned, and is admitted under
        // the policy artifact the Operator root carries -- absent which the
        // resolution is deny-all
        // (docs/decisions/2026-08-24-the-annotation-policy-is-the-operators.md).
        //
        // `cancellation` is the caller's process stop token rather than
        // anything this lifecycle holds: what interrupts an annotation session
        // is a person at the terminal that started it.
        //
        // The returned session borrows nothing from this object beyond what it
        // was handed, but its TaskContext is what every Tool call this
        // lifecycle issues for that session must be driven against, so it must
        // not outlive this lifecycle.
        [[nodiscard]]
        auto startExplorationSession(
            task::TaskRunConfig config,
            std::stop_token cancellation
        ) -> Result<std::unique_ptr<task::ExplorationSession>>;

        // Runs a Tool through the same durable Tool Runtime seam every actor
        // adapter uses. Namespace ownership selects the Framework or Project
        // catalog; the caller does not select a dispatch path. Exact terminal
        // replay returns without
        // recapturing, waiting, delivering, or consulting provider code.
        //
        // A mutating descriptor carries a mutation proposal built from the
        // descriptor's own effect bounds and this run's plan authority, and is
        // refused outright once the lifecycle has given up its lease. A
        // call whose canonical arguments carry an `observation_reference` is
        // issued against the reference this run minted for those exact bytes,
        // so a call consuming an observation this run never produced is refused
        // before it can occupy a durable coordinate at all.
        [[nodiscard]]
        auto invokeTool(
            ToolRootCall request,
            task::TaskContext& context
        ) -> Result<operator_runtime::ToolCallReplay>;

        // The three actor transports, each one translation away from the same
        // admitted request and the same dispatch. What differs between them is
        // what the transport delivers -- a model's parsed tool-use block, a
        // person's text, a Project's own bound entry -- and nothing after the
        // translation differs at all.
        //
        // A Tool this Project bound runs on its scoped program; a Tool in the
        // `framework` namespace is answered by this lifecycle's own providers.
        // Neither the caller nor this seam chooses which: the name's namespace
        // owns it, and ToolStartCatalog resolves it.
        [[nodiscard]]
        auto invokeAgentTool(
            operator_runtime::AgentToolUse const& use,
            task::TaskContext& context
        ) -> Result<operator_runtime::ToolCallReplay>;

        [[nodiscard]]
        auto invokeHumanTool(
            operator_runtime::HumanToolCommand const& command,
            task::TaskContext& context
        ) -> Result<operator_runtime::ToolCallReplay>;

        [[nodiscard]]
        auto startProjectAutomation(
            operator_runtime::ProjectAutomationStart const& start,
            task::TaskContext& context
        ) -> Result<operator_runtime::ToolCallReplay>;

        [[nodiscard]]
        auto wait(
            operator_runtime::SubscriptionCursor after,
            uint32 maximumEvents
        ) -> Result<operator_runtime::SubscriptionRead>;

        [[nodiscard]] auto shutdown() -> Status;
    };

    // Runs the Operator root's reclamation pass over runtimeDirectory and
    // reports what it removed.
    //
    // It opens a Coordinator of its own and closes it again, which is what
    // makes the pass safe to offer: claimExclusiveOwnership refuses this call
    // for as long as a session holds the root, so a sweep can never run beside
    // the publication or the dispatch it would sweep out from under. That
    // refusal is the whole concurrency argument -- there is no second one here.
    //
    // It is not folded into ProductLifecycle for two reasons the code states.
    // The sweep removes directories, and every verb that starts a lifecycle --
    // including the read-only observation path -- would then delete bytes from
    // a root it was handed to read. And the two counts below are a result: a
    // lifecycle hook has no caller to hand them to and would drop the only
    // report the pass produces.
    [[nodiscard]]
    auto reclaimRuntimeArtifacts(std::filesystem::path const& runtimeDirectory)
        -> Result<operator_runtime::ReclaimedRuntimeArtifacts>;

    // The production door for a RuntimeArtifact release upgrade: what a caller
    // states, in the order the ledger consumes it.
    //
    // One directory and one hash, because there is one read. artifactRootHash
    // is what the install holds artifactDirectory's own manifest bytes against
    // AND what the session the upgrade pins binds its SessionManifest to; the
    // ledger proves the two uses agree by refusing a pin whose manifest names
    // a root that was not installed.
    struct RuntimeUpgradeStart final
    {
        std::filesystem::path    projectDirectory{};
        std::filesystem::path    runtimeDirectory{};
        std::filesystem::path    artifactDirectory{};
        ContentHash              artifactRootHash;
        std::vector<std::string> controllerCapabilities{};
    };

    struct RuntimeUpgradeResult final
    {
        uint64      installedGeneration{};
        ContentHash artifactRootHash;
        std::string sessionId{};
    };

    // Publishes the RuntimeArtifact at artifactDirectory into the Operator
    // root at runtimeDirectory and pins the session that records the release.
    // The project at
    // projectDirectory names the deployment the upgrade session registers and
    // pins itself to, and the SessionManifest is derived exactly as
    // ProductLifecycle::start derives it -- same published schema, same policy
    // bytes, same agent profile -- with the candidate artifactRootHash where
    // start would put the installed root's.
    //
    // The generation the install compare-and-swaps against is read from the
    // root's active pin. Every Operator root holds one from its first open:
    // the genesis generation is part of its layout, and it is generation 0.
    // A root that has never been upgraded therefore compares against 0 and
    // its first real installation lands at 1 -- genesis is layout, not a
    // release, and never consumes that first number.
    [[nodiscard]]
    auto upgradeRuntimeArtifactAndPinSession(RuntimeUpgradeStart const& upgrade)
        -> Result<RuntimeUpgradeResult>;

    // Records the evidence that expanding a session's capability set onto
    // artifactRootHash was authorised, in the Operator root at
    // runtimeDirectory. The approval is recorded before the pin that needs
    // it, which is the ledger's whole rule: a pin that expands capabilities
    // without a recorded approval is refused, and this is the only production
    // door that records one.
    [[nodiscard]]
    auto approveReleaseCapabilities(
        std::filesystem::path const& runtimeDirectory,
        operator_runtime::ReleaseCapabilityApproval const& approval
    ) -> Status;

    // Which failure a caller is told about when the work and the close that
    // followed it both failed. The work's failure is the one a caller can act
    // on, so it stays primary and the close failure is added to it as context
    // rather than replacing it; a close that fails on its own is the only
    // failure there is and is reported as itself. Written once here rather than
    // at each site that closes a lifecycle, because a second spelling of this
    // rule is a second answer to the same question.
    template <typename Value>
    [[nodiscard]]
    auto reportAfterClose(Result<Value> outcome, Status closed) -> Result<Value>
    {
        if (!outcome.has_value())
        {
            if (closed.has_value())
            {
                return outcome;
            }
            auto error = std::move(outcome).error();
            error.addContext(
                "the product lifecycle also failed to close: "
                + std::string{closed.error().message()}
            );
            return std::unexpected{std::move(error)};
        }
        UF_TRY(std::move(closed));
        return outcome;
    }
}
