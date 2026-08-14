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

    [[nodiscard]]
    auto offeredProductTools(
        operator_runtime::OperatorCoordinator& coordinator,
        operator_runtime::ControllerBinding const& controller,
        operator_runtime::ProjectToolCatalogSchemaOwner const& catalog
    ) -> Result<std::vector<operator_runtime::OfferedTool>>;

    struct ProductStart final
    {
        std::filesystem::path    projectDirectory{};
        std::filesystem::path    runtimeDirectory{};
        std::string              authenticatedControllerId{};
        std::vector<std::string> controllerCapabilities{};
        std::string              controlledTargetId{};
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

    // The sole production construction site for Operator. The exact published
    // Operator protocol schema has a production reader here: its catalog bytes
    // are pinned into SessionManifest and passed to OperatorPlanAuthority with
    // deployment's production PlanProposal and StepIntent readers. No other
    // production type constructs an OperatorCoordinator, so every lifecycle
    // path below shares one persistence and recovery chain.
    class ProductLifecycle final
    {
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        explicit ProductLifecycle(std::unique_ptr<Impl> implementation);

    public:
        ProductLifecycle(ProductLifecycle&&) noexcept;
        auto operator=(ProductLifecycle&&) noexcept -> ProductLifecycle&;
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
        auto offeredTools() -> Result<std::vector<operator_runtime::OfferedTool>>;

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
