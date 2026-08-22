#pragma once

#include "ledger.hpp"

#include <functional>
#include <span>

namespace uf::operator_runtime
{
    // Provider code receives only the immutable call position after the
    // Coordinator has durably crossed the dispatch boundary. The executor
    // converts a returned domain error into an exact terminal-failure payload,
    // so no returned error strands a dispatched call without a classification.
    using ReadOnlyToolProvider =
        std::function<Result<ToolCallCompletion>(ToolCallPositionIdentity const&)>;

    // A mutating provider may prove delivery, prove absence, or report
    // uncertainty. Any returned error or terminal-failure completion is
    // conservatively persisted as possible because provider code ran only
    // after the durable dispatch boundary.
    using MutatingToolProvider =
        std::function<Result<ToolCallCompletion>(ToolCallPositionIdentity const&)>;

    // The one execution seam shared by all caller adapters. It first replays a
    // recorded outcome without authority or provider execution; only the first
    // new call proceeds through admission, durable dispatch and one provider
    // invocation.
    //
    // delegation is the optional non-owning observation of the grant a child
    // call stands on, and nullptr for a call the run's own context issued. It
    // is a call-scoped borrow: the executor hands it to admission and retains
    // nothing.
    class ToolRuntimeExecutor final
    {
        OperatorCoordinator& m_coordinator;

    public:
        explicit ToolRuntimeExecutor(OperatorCoordinator& coordinator) noexcept;

        [[nodiscard]]
        auto invokeReadOnly(
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            ToolDelegationGrant const* delegation,
            ReadOnlyToolProvider const& provider
        ) -> Result<ToolCallReplay>;

        [[nodiscard]]
        auto invokeMutating(
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            ToolDelegationGrant const* delegation,
            OperatorPlanAuthority const& planAuthority,
            std::span<ProposedEffect const> effects,
            std::span<ToolApprovalGrant const> approvals,
            MutatingToolProvider const& provider
        ) -> Result<ToolCallReplay>;
    };
}
