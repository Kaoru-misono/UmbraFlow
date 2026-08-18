#pragma once

#include "ledger.hpp"

#include <task/task-host.hpp>

#include <core/error/result.hpp>

#include <memory>
#include <optional>
#include <string>

namespace uf::operator_runtime
{
    // The production join between the Operator ledger and Host delivery for
    // one controlled target. Lease acquire/release/takeover and a complete
    // dispatch share m_targetSerialization inside the implementation, so a
    // displaced fence cannot enter reserveDispatch after takeover returns or
    // move between a successful reservation and TaskHost::deliver.
    class OperatorTaskHost final
    {
        struct Impl;
        std::unique_ptr<Impl> m_impl;

        explicit OperatorTaskHost(std::unique_ptr<Impl> implementation);

        [[nodiscard]]
        auto requireControlledTarget(std::string const& controlledTargetId) const
            -> Status;

    public:
        struct DispatchResult final
        {
            DispatchReservation      reservation;
            task::HostDeliveryReport delivery;
            StoredOperation          operation;
        };

        [[nodiscard]]
        static auto create(
            OperatorCoordinator coordinator,
            std::string controlledTargetId
        ) -> Result<OperatorTaskHost>;

        OperatorTaskHost(OperatorTaskHost&&) noexcept;
        auto operator=(OperatorTaskHost&&) noexcept -> OperatorTaskHost&;
        OperatorTaskHost(OperatorTaskHost const&) = delete;
        auto operator=(OperatorTaskHost const&) -> OperatorTaskHost& = delete;
        ~OperatorTaskHost();

        [[nodiscard]]
        auto controlledTargetId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        // Non-control Coordinator work remains available through the authority
        // that owns it. Production control changes and delivery use the joined
        // operations below; direct Coordinator control calls are test/setup
        // surface and do not update this Host's fence.
        [[nodiscard]] auto coordinator() noexcept -> OperatorCoordinator&;
        [[nodiscard]] auto host() noexcept -> task::TaskHost&;

        [[nodiscard]]
        auto acquireLease(
            ControllerBinding const& controller
        ) -> Result<ControlLease>;

        [[nodiscard]]
        auto releaseLease(ControlLease const& lease) -> Status;

        [[nodiscard]]
        auto takeoverLease(
            ControllerBinding const& controller,
            std::string const& reason
        ) -> Result<ControlTakeover>;

        [[nodiscard]]
        auto dispatch(
            std::string const& operationId,
            uint64 expectedRevision,
            ControlLease const& lease,
            GenerationId runtimeGeneration,
            AuthorityDecisionId const& authorityDecisionId,
            std::optional<ApprovalGrant> const& approval,
            task::TaskContext& context
        ) -> Result<DispatchResult>;
    };
}
