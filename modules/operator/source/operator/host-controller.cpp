#include "host-controller.hpp"

#include <domain/error.hpp>

#include <mutex>
#include <utility>

namespace uf::operator_runtime
{
    struct OperatorTaskHost::Impl final
    {
        OperatorCoordinator coordinator;
        task::TaskHost      host{};
        std::string         controlledTargetId;
        std::mutex          targetSerialization{};

        Impl(
            OperatorCoordinator ownedCoordinator,
            std::string ownedControlledTargetId
        )
            : coordinator{std::move(ownedCoordinator)}
            , controlledTargetId{std::move(ownedControlledTargetId)}
        {
        }
    };

    OperatorTaskHost::OperatorTaskHost(std::unique_ptr<Impl> implementation)
        : m_impl{std::move(implementation)}
    {
    }

    auto OperatorTaskHost::create(
        OperatorCoordinator coordinator,
        std::string controlledTargetId
    ) -> Result<OperatorTaskHost>
    {
        if (controlledTargetId.empty())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "an Operator TaskHost must name its controlled target"
            );
        }
        return OperatorTaskHost{
            std::make_unique<Impl>(
                std::move(coordinator),
                std::move(controlledTargetId)
            )
        };
    }

    OperatorTaskHost::OperatorTaskHost(OperatorTaskHost&&) noexcept = default;

    auto OperatorTaskHost::operator=(OperatorTaskHost&&) noexcept
        -> OperatorTaskHost& = default;

    OperatorTaskHost::~OperatorTaskHost() = default;

    auto OperatorTaskHost::requireControlledTarget(
        std::string const& controlledTargetId
    ) const -> Status
    {
        if (controlledTargetId != m_impl->controlledTargetId)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "this Operator TaskHost is bound to another controlled target"
            );
        }
        return ok();
    }

    auto OperatorTaskHost::controlledTargetId() const noexcept
        -> std::string const&
    {
        return m_impl->controlledTargetId;
    }

    auto OperatorTaskHost::coordinator() noexcept -> OperatorCoordinator&
    {
        return m_impl->coordinator;
    }

    auto OperatorTaskHost::host() noexcept -> task::TaskHost&
    {
        return m_impl->host;
    }

    auto OperatorTaskHost::acquireLease(
        ControllerBinding const& controller
    ) -> Result<ControlLease>
    {
        UF_TRY(requireControlledTarget(controller.controlledTargetId()));
        auto lock = std::scoped_lock{m_impl->targetSerialization};
        UF_TRY_VALUE(lease, m_impl->coordinator.acquireLease(controller));
        UF_TRY(m_impl->host.adoptControlFence(controlFence(lease)));
        return lease;
    }

    auto OperatorTaskHost::takeoverLease(
        ControllerBinding const& controller,
        std::string const& reason
    ) -> Result<ControlTakeover>
    {
        UF_TRY(requireControlledTarget(controller.controlledTargetId()));
        auto lock = std::scoped_lock{m_impl->targetSerialization};
        UF_TRY_VALUE(takeover, m_impl->coordinator.takeoverLease(controller, reason));
        UF_TRY(m_impl->host.adoptControlFence(controlFence(takeover.lease)));
        return takeover;
    }

    auto OperatorTaskHost::dispatch(
        std::string const& operationId,
        uint64 expectedRevision,
        ControlLease const& lease,
        GenerationId runtimeGeneration,
        AuthorityDecisionId const& authorityDecisionId,
        std::optional<ApprovalGrant> const& approval,
        task::TaskContext& context
    ) -> Result<DispatchResult>
    {
        UF_TRY(requireControlledTarget(lease.controlledTargetId));
        auto lock = std::scoped_lock{m_impl->targetSerialization};
        UF_TRY_VALUE(
            reservation,
            m_impl->coordinator.reserveDispatch(
                operationId,
                expectedRevision,
                lease,
                runtimeGeneration,
                authorityDecisionId,
                approval
            )
        );
        UF_TRY_VALUE(
            delivery,
            m_impl->host.deliver(
                reservation.authority,
                context
            )
        );
        UF_TRY_VALUE(
            operation,
            m_impl->coordinator.recordDeliveryOutcome(
                lease,
                reservation.operationRevision,
                delivery
            )
        );
        return DispatchResult{
            .reservation = std::move(reservation),
            .delivery    = std::move(delivery),
            .operation   = std::move(operation),
        };
    }
}
