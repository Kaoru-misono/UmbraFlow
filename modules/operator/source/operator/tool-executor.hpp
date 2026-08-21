#pragma once

#include "ledger.hpp"

#include <functional>

namespace uf::operator_runtime
{
    // Provider code receives only the immutable call position after the
    // Coordinator has durably crossed the dispatch boundary. The executor
    // converts a returned domain error into an exact terminal-failure payload,
    // so no returned error strands a dispatched call without a classification.
    using ReadOnlyToolProvider =
        std::function<Result<ToolCallCompletion>(ToolCallPositionIdentity const&)>;

    // The one execution seam shared by all caller adapters. It first replays a
    // recorded outcome without authority or provider execution; only the first
    // new call proceeds through admission, durable dispatch and one provider
    // invocation.
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
            ReadOnlyToolProvider const& provider
        ) -> Result<ToolCallReplay>;
    };
}
