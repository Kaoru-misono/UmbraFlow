#pragma once

#include <task/task-context.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>

#include <trace/event.hpp>

#include <optional>
#include <string_view>

namespace uf::task
{
    // What every consumer of the private capability surface writes when it uses
    // one: the shape of a task.native_call line, and what happens when the sink
    // loses one. It lives apart from the primitives themselves because a second
    // copy would let one caller's evidence drift from another's, and a trace
    // whose halves are written to different rules cannot be read as one stream.
    //
    // There was a second consumer at this level -- an operator sending commands
    // from outside, through the retired `OperatorSession` -- which is why this
    // is a file rather than a section of ffi/uf-tables.cpp. It stays that way:
    // the rule is about the line, not about who happens to write it today.

    // Which primitive ran and what it was handed. Every task.native_call carries
    // it, so a primitive the host fails before the engine is reached -- a ticket or
    // a hit naming a cycle that is no longer open, which is the only failure that
    // produces no engine event at all -- still names the cycle the caller tried to
    // use. A call-scoped parameter type: `verb` views a string literal and the
    // struct never outlives the call that builds it.
    struct NativeCallIdentity final
    {
        std::string_view      verb;
        std::optional<uint64> cycleOrdinal{};
        std::optional<uint64> hitCycleOrdinal{};

        // The duration this call was handed, in whole milliseconds: the pause a
        // settle declared, or the hold a long press named. One field rather than
        // two because a line carries at most one duration and the verb already
        // says which, exactly as `contentHash` serves four verbs.
        //
        // A settle reaches no engine verb, so this is the only evidence it
        // happened and a replay cannot reconstruct the run without it. A long
        // press records the hold here as well for `key`'s reason: a press the host
        // refuses before the engine is reached produces no engine line at all, and
        // the hold may be exactly what the refusal was about.
        std::optional<uint64> durationMillis{};

        // The key a `key` call was handed. Recorded on the native call as well as
        // on the engine's own delivery line, because a `key` the host refuses
        // before the engine is reached produces no engine line at all and the name
        // is what the refusal was about.
        std::optional<KeyName> key{};

        // The detent count a `cycle_scroll` call was handed, recorded on `key`'s
        // reasoning.
        std::optional<int32> wheelNotches{};

        // The project file a project_read or project_write named, and how many
        // bytes crossed. Views, like `verb`: the struct never outlives the call
        // that builds it, and both spellings live on that call's stack.
        std::optional<std::string_view> resourceName{};
        std::optional<uint64>           byteCount{};

        // The SHA-256 the call is about: the bytes a project verb moved, the blob
        // a template_load decoded, or the pixels a cycle_crop encoded. It is one
        // field rather than three because a line carries at most one of them and
        // the verb already says which.
        std::optional<std::string_view> contentHash{};
    };

    [[nodiscard]]
    auto nativeCallEvent(
        NativeCallIdentity const& call,
        trace::NativeCallOutcome outcome,
        std::optional<AutomationErrorKind> errorKind
    ) -> trace::TraceEvent;

    // Records a completed native call. A sink failure becomes a Tier B IoFailure
    // that aborts the verb: losing trace evidence is a hard error, not a silent
    // drop, matching the engine's throw-instant emit discipline.
    [[nodiscard]]
    auto recordNativeCall(
        TaskContext& context,
        NativeCallIdentity const& call,
        trace::NativeCallOutcome outcome
    ) -> Status;

    // Records a failed native call. It reports nothing, and that is the contract:
    // the caller already holds the verb's own error and surfaces THAT. A caller
    // asking why its click failed must not be told the trace file was unwritable,
    // and for a cancellation the Tier C sentinel has to stay on the raise path.
    // The sink failure is latched on the context, where the run's owner reads it
    // afterwards rather than losing it silently.
    auto recordNativeCallFailure(
        TaskContext& context,
        NativeCallIdentity const& call,
        Error const& error
    ) -> void;

    // Whether this generation is still live, or the failure that spent it. Both
    // front-ends ask this before every primitive, so a caller that swallowed what
    // was raised cannot drive one more engine verb before the generation is torn
    // down. It re-reports under the kind that spent the generation rather than one
    // fixed value: a cancelled run and a run stopped by a framework bug are
    // different verdicts.
    [[nodiscard]] auto requireLiveGeneration(TaskContext const& context) -> Status;

    // Latches the generation terminal and reports the cancellation when the run's
    // single cancel source has requested a stop.
    //
    // The three time primitives need it and the observation and action primitives do
    // not: those reach the engine, which already fails closed on the same token,
    // while a sleep reaches nothing and would otherwise burn its whole budget on a
    // generation that is already over. It latches BEFORE reporting, which is what
    // makes the next primitive refuse at requireLiveGeneration even if the caller
    // swallowed this. The context is mutable because latching is the point of the
    // call rather than a side effect of a query.
    [[nodiscard]] auto requireNotCancelled(TaskContext& context) -> Status;
}
