#include "tool-executor.hpp"

#include <json/value.hpp>

#include <domain/error.hpp>

#include <string>
#include <utility>

namespace uf::operator_runtime
{
    namespace
    {
        [[nodiscard]]
        auto providerFailureCompletion(Error const& error)
            -> Result<ToolCallCompletion>
        {
            auto const kind = automationErrorKind(error);
            auto payload = json::Value::ofObject({
                {"failure_response",
                 json::Value::ofString(
                     std::string{
                         failureResponseWireName(failureResponse(error))
                     }
                 )},
                {"kind",
                 json::Value::ofString(
                     kind
                         ? std::string{automationErrorWireName(*kind)}
                         : std::string{"unclassified"}
                 )},
                {"message",
                 json::Value::ofString(std::string{error.message()})},
            });
            UF_TRY_VALUE(
                canonical,
                CanonicalJson::parseExact(json::canonicalBytes(payload))
            );
            return ToolCallCompletion::terminalFailure(std::move(canonical));
        }
    }

    ToolRuntimeExecutor::ToolRuntimeExecutor(
        OperatorCoordinator& coordinator
    ) noexcept
        : m_coordinator{coordinator}
    {
    }

    auto ToolRuntimeExecutor::invokeReadOnly(
        ControllerBinding const& controller,
        ControlLease const& lease,
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call,
        ReadOnlyToolProvider const& provider
    ) -> Result<ToolCallReplay>
    {
        if (!provider)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Read-only Tool invocation requires a provider"
            );
        }

        UF_TRY(m_coordinator.persistToolRootRequest(root));
        UF_TRY(m_coordinator.persistToolCallPosition(root, call));
        UF_TRY_VALUE(replay, m_coordinator.replayToolCall(root, call));
        switch (replay.state)
        {
        case ToolCallState::Confirmed:
        case ToolCallState::ProvenAbsent:
        case ToolCallState::Possible:
        case ToolCallState::Rejected:
        case ToolCallState::TerminalFailure:
        case ToolCallState::TerminallyUnresolved:
            return replay;
        case ToolCallState::Dispatching:
            return fail(
                AutomationErrorKind::ActionRejected,
                "Tool call is already dispatching"
            );
        case ToolCallState::Proposed:
        case ToolCallState::Admitted:
            break;
        }

        UF_TRY_VALUE(
            admission,
            m_coordinator.admitReadOnlyToolCall(controller, lease, root, call)
        );
        UF_TRY_VALUE(
            dispatch,
            m_coordinator.beginToolCallDispatch(admission)
        );
        auto provided = provider(call);
        auto classified = [&provided]() -> Result<ToolCallCompletion>
        {
            if (provided)
            {
                return std::move(*provided);
            }
            return providerFailureCompletion(provided.error());
        }();
        UF_TRY_VALUE(completion, std::move(classified));
        UF_TRY(m_coordinator.completeToolCallDispatch(dispatch, completion));
        return m_coordinator.replayToolCall(root, call);
    }
}
