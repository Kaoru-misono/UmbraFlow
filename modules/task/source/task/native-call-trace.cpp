#include "native-call-trace.hpp"

#include <task/task-context.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>

#include <domain/error.hpp>

#include <trace/event.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace uf::task
{
    namespace
    {
        // The automation kind of a failure, defaulting an unclassified error to
        // InternalInvariant. It mirrors what the Luau Tier ladder will raise, so the
        // recorded kind is exactly the kind the caller is about to be told.
        [[nodiscard]]
        auto kindOf(Error const& error) noexcept -> AutomationErrorKind
        {
            return automationErrorKind(error)
                .value_or(AutomationErrorKind::InternalInvariant);
        }
    }

    auto nativeCallEvent(
        NativeCallIdentity const& call,
        trace::NativeCallOutcome outcome,
        std::optional<AutomationErrorKind> errorKind
    ) -> trace::TraceEvent
    {
        return trace::TraceEvent{
            .kind       = trace::TraceEventKind::TaskNativeCall,
            .nativeCall = trace::TraceEvent::NativeCall{
                .verb            = std::string{call.verb},
                .outcome         = outcome,
                .cycleOrdinal    = call.cycleOrdinal,
                .hitCycleOrdinal = call.hitCycleOrdinal,
                .durationMillis  = call.durationMillis,
                .resourceName    = call.resourceName.transform(
                    [](std::string_view name) -> std::string
                    {
                        return std::string{name};
                    }
                ),
                .byteCount   = call.byteCount,
                .contentHash = call.contentHash.transform(
                    [](std::string_view hash) -> std::string
                    {
                        return std::string{hash};
                    }
                ),
            },
            .errorKind = errorKind,
            .key       = call.key,
        };
    }

    auto recordNativeCall(
        TaskContext& context,
        NativeCallIdentity const& call,
        trace::NativeCallOutcome outcome
    ) -> Status
    {
        return context.emitTrace(nativeCallEvent(call, outcome, std::nullopt));
    }

    auto recordNativeCallFailure(
        TaskContext& context,
        NativeCallIdentity const& call,
        Error const& error
    ) -> void
    {
        auto status = context.emitTrace(
            nativeCallEvent(call, trace::NativeCallOutcome::Failed, kindOf(error))
        );
        if (!status)
        {
            context.latchTraceFailure();
        }
    }

    auto requireLiveGeneration(TaskContext const& context) -> Status
    {
        auto const terminal = context.terminalKind();
        if (!terminal.has_value())
        {
            return ok();
        }
        if (*terminal == AutomationErrorKind::Cancelled)
        {
            return fail(AutomationErrorKind::Cancelled, "the task generation is cancelled");
        }
        return fail(
            *terminal,
            "the task generation is spent after a framework invariant failure"
        );
    }

    auto requireNotCancelled(TaskContext& context) -> Status
    {
        if (!context.cancellationRequested())
        {
            return ok();
        }
        context.markTerminal(AutomationErrorKind::Cancelled);
        return fail(AutomationErrorKind::Cancelled, "the task generation is cancelled");
    }
}
