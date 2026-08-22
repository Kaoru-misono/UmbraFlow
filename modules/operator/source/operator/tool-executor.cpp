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
    }

    ToolRuntimeExecutor::ToolRuntimeExecutor(
        OperatorCoordinator& coordinator
    ) noexcept
        : m_coordinator{coordinator}
    {
    }

    auto ToolRuntimeExecutor::invoke(
        ToolAdmissionRequest const& request,
        ToolProvider const& provider
    ) -> Result<ToolCallReplay>
    {
        if (!provider)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Tool invocation requires a provider"
            );
        }

        auto const& root = request.root;
        auto const& call = request.call;
        auto const requiredMutability = request.requiredMutability();

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
        case ToolCallState::Proposed:
        case ToolCallState::Admitted:
            break;
        }

        // A dispatching row is an incarnation that died inside its own
        // dispatch. Re-entry is the ledger's decision, not this seam's: it
        // refuses only a mutating leaf, whose interrupted provider may have
        // moved the world with no record of it, and admits everything whose
        // re-execution cannot deliver anything a durable row does not already
        // classify. Everything else enters admission for the first or the next
        // time.
        auto dispatched = replay.state == ToolCallState::Dispatching
            ? m_coordinator.reenterToolCallDispatch(
                  request.controller,
                  request.lease,
                  root,
                  call
              )
            : [this, &request]() -> Result<ToolCallDispatch>
              {
                  UF_TRY_VALUE(admitted, m_coordinator.admitToolCall(request));
                  return m_coordinator.beginToolCallDispatch(admitted);
              }();
        UF_TRY_VALUE(dispatch, std::move(dispatched));
        // Whether this call's outcome is allowed to be uncertain is the same
        // question the restart, the re-entry gate and the completion refusal
        // ask, so it is the same predicate and not a second reading of
        // mutability. A composed call's whole effect surface is children that
        // already carry their own classification, so its failure is a fact
        // about the frame and stays terminal; only a mutating leaf's failure
        // could be hiding an effect no row records, and only that is converted.
        //
        // The composition comes off the coordinate the runtime minted, so no
        // durable read is needed here: the provider identity inside it is the
        // same one persistToolCallPosition wrote the provider_kind column from.
        auto const effectMayBeUnrecorded = toolCallEffectMayBeUnrecorded(
            toolEffectComposition(call.provider()),
            requiredMutability
        );
        auto provided = provider(call);
        auto classified = [&provided, effectMayBeUnrecorded]()
            -> Result<ToolCallCompletion>
        {
            if (provided)
            {
                auto completion = std::move(*provided);
                if (
                    effectMayBeUnrecorded
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
            return effectMayBeUnrecorded
                ? providerPossibleCompletion(provided.error())
                : providerFailureCompletion(provided.error());
        }();
        UF_TRY_VALUE(completion, std::move(classified));
        UF_TRY(m_coordinator.completeToolCallDispatch(dispatch, completion));
        return m_coordinator.replayToolCall(root, call);
    }
}
