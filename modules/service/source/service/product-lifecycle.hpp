#pragma once

#include <operator/host-controller.hpp>
#include <operator/ledger.hpp>
#include <operator/tool-invocation.hpp>

#include <task/task-context.hpp>
#include <task/runtime-model-file.hpp>
#include <task/ui-observation.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::service
{
    enum class LifecycleAccess : uint8
    {
        ReadOnly,
        Writable,
    };

    [[nodiscard]]
    auto lifecycleAccessAfterRestart(
        std::span<operator_runtime::RecoveredUncertainDispatch const> recoveries
    ) noexcept -> LifecycleAccess;

    struct ProductStart final
    {
        std::filesystem::path    projectDirectory{};
        std::filesystem::path    runtimeDirectory{};
        std::string              authenticatedControllerId{};
        std::vector<std::string> controllerCapabilities{};
        std::string              controlledTargetId{};

        // The observed-instance world this session observes in. It is
        // transferred into the session pin unchanged, so the observations this
        // lifecycle produces are bound to one scope.
        operator_runtime::ObservedInstanceWorldScope worldScope;
    };

    struct ProductExecution final
    {
        operator_runtime::StoredOperation       operation{};
        std::optional<task::HostDeliveryReport> delivery{};
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

    // The production session over an Operator root. The exact published
    // Operator protocol schema has a production reader here: its catalog bytes
    // are pinned into SessionManifest and passed to OperatorPlanAuthority with
    // deployment's production PlanProposal and StepIntent readers.
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
        auto recoveries() const
            -> std::vector<operator_runtime::RecoveredUncertainDispatch>;

        [[nodiscard]]
        auto observe(task::TaskContext& context)
            -> Result<ProductObservation>;

        [[nodiscard]]
        auto execute(
            operator_runtime::SnapshotRecord const& snapshot,
            std::string toolName,
            std::string exactArgumentsJcs,
            std::string clientRequestId,
            task::TaskContext& context
        ) -> Result<ProductExecution>;

        [[nodiscard]]
        auto wait(
            operator_runtime::SubscriptionCursor after,
            uint32 maximumEvents
        ) -> Result<operator_runtime::SubscriptionRead>;

        [[nodiscard]]
        auto reconcile(operator_runtime::ReconciliationCommit const& commit)
            -> Result<operator_runtime::StoredOperation>;

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
    // The two hashes are both stated because the ledger's two reads cannot be
    // derived from one another from this module's side of the boundary.
    // expectedReleaseManifestHash is what installRuntimeArtifact compares the
    // handoff's release.manifest.json against, and artifactRootHash is the root
    // that manifest declares -- which is what the session the upgrade pins
    // binds its SessionManifest to. The ledger itself proves them consistent:
    // the pin is refused unless the manifest's root is the installed one, and
    // the installed root is the one the trusted release manifest named.
    struct RuntimeUpgradeStart final
    {
        std::filesystem::path    projectDirectory{};
        std::filesystem::path    runtimeDirectory{};
        std::filesystem::path    handoffRoot{};
        ContentHash              expectedReleaseManifestHash;
        ContentHash              artifactRootHash;
        std::vector<std::string> controllerCapabilities{};
    };

    struct RuntimeUpgradeResult final
    {
        uint64      installedGeneration{};
        ContentHash artifactRootHash;
        std::string sessionId{};
    };

    // Publishes the handoff into the Operator root at runtimeDirectory and
    // pins the session that records the release. The project at
    // projectDirectory names the deployment the upgrade session registers and
    // pins itself to, and the SessionManifest is derived exactly as
    // ProductLifecycle::start derives it -- same published schema, same policy
    // bytes, same agent profile -- with the candidate artifactRootHash where
    // start would put the installed root's.
    //
    // The generation the install compare-and-swaps against is read from the
    // root's active pin, and a root with no active release is the bootstrap
    // case: there is nothing to compare against but the absence of anything,
    // which is what generation 0 spells (the schema's first installation
    // starts at 1).
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
