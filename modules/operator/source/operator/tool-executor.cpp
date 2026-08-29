#include "tool-executor.hpp"

#include <json/value.hpp>

#include <core/error/contracts.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <chrono>
#include <format>
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
                {"code",
                 json::Value::ofString(
                     kind
                         ? std::string{automationErrorWireName(*kind)}
                         : std::string{"unclassified"}
                 )},
                {"message",
                 json::Value::ofString(std::string{error.message()})},
                {"retryable", json::Value::ofBoolean(false)},
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

        // Whole milliseconds, because that is the unit the ceiling is declared
        // in and a comparison between two units would be a second reading of
        // one number.
        [[nodiscard]]
        auto elapsedMillisSince(MonotonicInstant startedAt) noexcept -> uint64
        {
            auto const elapsed = MonotonicInstant::now().saturatingDurationSince(
                startedAt
            );
            auto const count =
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                    .count();
            return count <= 0 ? uint64{0U} : static_cast<uint64>(count);
        }

        // The sentence a crossed ceiling reports. It names WHAT was exceeded
        // and WHAT the limit was, because a Project cannot act on a signal that
        // says only that something failed; the same bytes reach the durable
        // payload and the error that stops a run, so the two cannot say
        // different things about one overrun.
        [[nodiscard]]
        auto timeoutMessage(
            ToolCallPositionIdentity const& call,
            uint64 elapsedMillis
        ) -> std::string
        {
            return std::format(
                "{} exceeded the maximum_elapsed_ms of {} its Tool declared: "
                "the call returned after {} ms",
                call.toolName(),
                call.descriptor().timeout.maximumElapsedMillis,
                elapsedMillis
            );
        }

        [[nodiscard]]
        auto timeoutPayload(
            ToolCallPositionIdentity const& call,
            uint64 elapsedMillis
        ) -> Result<CanonicalJson>
        {
            auto payload = json::Value::ofObject({
                {"code",
                 json::Value::ofString(
                     std::string{
                         automationErrorWireName(AutomationErrorKind::Timeout)
                     }
                 )},
                {"message",
                 json::Value::ofString(timeoutMessage(call, elapsedMillis))},
                {"retryable", json::Value::ofBoolean(false)},
            });
            UF_TRY_VALUE(
                canonical,
                CanonicalJson::parseExact(json::canonicalBytes(payload))
            );
            return canonical;
        }

        // The outcome a call that crossed its own declared ceiling gets. It is
        // a failed call whatever the provider went on to answer: the Project
        // stated that this Tool must not take longer, and an answer that
        // arrived late is not the answer it declared. The mutating-leaf
        // conversion is the one a provider failure gets, for its own reason.
        [[nodiscard]]
        auto timeoutCompletion(
            ToolCallPositionIdentity const& call,
            uint64 elapsedMillis,
            bool effectMayBeUnrecorded
        ) -> Result<ToolCallCompletion>
        {
            UF_TRY_VALUE(payload, timeoutPayload(call, elapsedMillis));
            return effectMayBeUnrecorded
                ? ToolCallCompletion::possible(std::move(payload))
                : ToolCallCompletion::terminalFailure(std::move(payload));
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
                  request
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
        // The declared wall clock, enforced here because this is the one place
        // every caller adapter's call passes through a provider. It is judged
        // when the provider returns rather than while it runs: a provider is
        // synchronous on this thread, so there is nothing here that could take
        // a running one away from it, and what a declared ceiling can still
        // decide is the outcome of the call that crossed it and whether the run
        // goes on.
        auto const startedAt     = MonotonicInstant::now();
        auto provided            = provider(call);
        auto const elapsedMillis = elapsedMillisSince(startedAt);
        UF_TRY(m_coordinator.ensureToolRunIsLive(call));
        UF_TRY_VALUE(unresolved, m_coordinator.hasUnresolvedToolDescendants(call));
        if (unresolved)
        {
            // The child owns uncertainty; the enclosing frame remains
            // replayable and cannot hide it behind success or failure.
            return m_coordinator.replayToolCall(root, call);
        }
        auto const& timeout      = call.descriptor().timeout;
        auto const overran       = elapsedMillis > timeout.maximumElapsedMillis;

        auto classified = overran
            ? timeoutCompletion(call, elapsedMillis, effectMayBeUnrecorded)
            : [&provided, effectMayBeUnrecorded]() -> Result<ToolCallCompletion>
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
        UF_TRY_VALUE(outcome, m_coordinator.replayToolCall(root, call));
        if (!overran)
        {
            return outcome;
        }

        // What the Project chose, carried out. The overrun is already durable
        // above, so both arms report it; they differ only in whether this run
        // is allowed to go on, which is the whole of what a framework can do
        // about a ceiling it did not choose.
        switch (timeout.onTimeout)
        {
        case TimeoutAction::Reobserve:
            // The run continues and the recorded timeout is its answer, so the
            // Project looks again rather than being torn down. A returned VALUE
            // is what "the run continues" means at every caller adapter and at
            // the scoped seam alike.
            return outcome;
        case TimeoutAction::Stop:
            // A returned FAILURE is terminal for a scoped run and is the
            // refusal every other adapter renders, so this is the stop the
            // Project asked for.
            return fail(
                AutomationErrorKind::Timeout,
                timeoutMessage(call, elapsedMillis)
            );
        }

        UF_UNREACHABLE_MSG("Unknown TimeoutAction value");
    }
}
