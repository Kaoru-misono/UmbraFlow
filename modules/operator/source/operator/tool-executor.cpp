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
        auto providerErrorPayload(Error const& error) -> Result<CanonicalJson>
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
            return canonical;
        }

        [[nodiscard]]
        auto providerFailureCompletion(Error const& error)
            -> Result<ToolCallCompletion>
        {
            UF_TRY_VALUE(payload, providerErrorPayload(error));
            return ToolCallCompletion::terminalFailure(std::move(payload));
        }

        [[nodiscard]]
        auto providerPossibleCompletion(Error const& error)
            -> Result<ToolCallCompletion>
        {
            UF_TRY_VALUE(payload, providerErrorPayload(error));
            return ToolCallCompletion::possible(std::move(payload));
        }

        [[nodiscard]]
        auto invokeTool(
            OperatorCoordinator& coordinator,
            ControllerBinding const& controller,
            ControlLease const& lease,
            ToolRootRequestIdentity const& root,
            ToolCallPositionIdentity const& call,
            ReadOnlyToolProvider const& provider,
            ToolMutability requiredMutability,
            OperatorPlanAuthority const* planAuthority,
            std::span<ProposedEffect const> effects,
            std::span<ToolApprovalGrant const> approvals
        ) -> Result<ToolCallReplay>
        {
            if (!provider)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "Tool invocation requires a provider"
                );
            }
            if (
                requiredMutability == ToolMutability::Mutating
                && planAuthority == nullptr
            )
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "Mutating Tool executor requires plan authority"
                );
            }

            UF_TRY(coordinator.persistToolRootRequest(root));
            UF_TRY(coordinator.persistToolCallPosition(root, call));
            UF_TRY_VALUE(replay, coordinator.replayToolCall(root, call));
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

            auto admission = requiredMutability == ToolMutability::ReadOnly
                ? coordinator.admitReadOnlyToolCall(
                      controller,
                      lease,
                      root,
                      call
                  )
                : coordinator.admitMutatingToolCall(
                      controller,
                      lease,
                      root,
                      call,
                      *planAuthority,
                      effects,
                      approvals
                  );
            UF_TRY_VALUE(admitted, std::move(admission));
            UF_TRY_VALUE(dispatch, coordinator.beginToolCallDispatch(admitted));
            auto provided = provider(call);
            auto classified = [&provided, requiredMutability]()
                -> Result<ToolCallCompletion>
            {
                if (provided)
                {
                    auto completion = std::move(*provided);
                    if (
                        requiredMutability == ToolMutability::Mutating
                        && completion.kind()
                            == ToolCallCompletionKind::TerminalFailure
                    )
                    {
                        return ToolCallCompletion::possible(
                            completion.payload(),
                            completion.evidence()
                        );
                    }
                    return completion;
                }
                return requiredMutability == ToolMutability::Mutating
                    ? providerPossibleCompletion(provided.error())
                    : providerFailureCompletion(provided.error());
            }();
            UF_TRY_VALUE(completion, std::move(classified));
            UF_TRY(coordinator.completeToolCallDispatch(dispatch, completion));
            return coordinator.replayToolCall(root, call);
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
        return invokeTool(
            m_coordinator,
            controller,
            lease,
            root,
            call,
            provider,
            ToolMutability::ReadOnly,
            nullptr,
            {},
            {}
        );
    }

    auto ToolRuntimeExecutor::invokeMutating(
        ControllerBinding const& controller,
        ControlLease const& lease,
        ToolRootRequestIdentity const& root,
        ToolCallPositionIdentity const& call,
        OperatorPlanAuthority const& planAuthority,
        std::span<ProposedEffect const> effects,
        std::span<ToolApprovalGrant const> approvals,
        MutatingToolProvider const& provider
    ) -> Result<ToolCallReplay>
    {
        return invokeTool(
            m_coordinator,
            controller,
            lease,
            root,
            call,
            provider,
            ToolMutability::Mutating,
            &planAuthority,
            effects,
            approvals
        );
    }
}
