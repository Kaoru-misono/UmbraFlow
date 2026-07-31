#pragma once

#include <task/task-context.hpp>

#include <core/error/error.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>

#include <trace/event.hpp>

#include <optional>
#include <string_view>

namespace uf::task
{
    // The private capability surface has TWO consumers at the same level: the
    // trusted Luau framework (through the primitives in ffi/uf-tables.cpp) and an
    // operator sending commands from outside (through OperatorSession). This
    // header is what they share.
    //
    // What they must NOT each own a copy of is exactly this: the shape of a
    // task.native_call line, and what happens when the sink loses one. A second
    // copy of either would let one front-end's evidence drift from the other's,
    // and a trace whose two halves are written to different rules cannot be read
    // as one stream.
    //
    // What they each keep is how a failure SURFACES -- the Luau side raises through
    // the Tier ladder, the operator side returns a Result and writes a result line
    // -- because that is a property of the front-end rather than of the guarantee.
    // The guarantee itself is neither side's: it lives in TaskContext's ledger and
    // below it in the engine's authorization, which both call.

    // Which primitive ran and what it was handed. Every task.native_call carries
    // it, so a primitive the host fails before the engine is reached -- a ticket or
    // a hit naming a cycle that is no longer open, which is the only failure that
    // produces no engine event at all -- still names the cycle the caller tried to
    // use. A call-scoped parameter type: `verb` views a string literal and the
    // struct never outlives the call that builds it.
    struct NativeCallIdentity final
    {
        std::string_view                     verb;
        std::optional<uint64>                cycleOrdinal{};
        std::optional<uint64>                hitCycleOrdinal{};
        std::optional<annotation::ElementId> elementId{};

        // The pause a settle declared, in whole milliseconds. A settle reaches no
        // engine verb, so this is the only evidence it happened, and a replay
        // cannot reconstruct the run without it.
        std::optional<uint64> durationMillis{};

        // The key a `key` call was handed. Recorded on the native call as well as
        // on the engine's own delivery line, because a `key` the host refuses
        // before the engine is reached produces no engine line at all and the name
        // is what the refusal was about.
        std::optional<KeyName> key{};
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
    // the caller already holds the verb's own error and surfaces THAT, however its
    // front-end surfaces failures.
    //
    // Deliberately not returning the sink's failure. A caller asking why its click
    // failed must not be told the trace file was unwritable, and for a cancellation
    // the Tier C sentinel has to stay on the raise path. The verb is failing either
    // way, so its own cause wins and the sink failure is latched on the context,
    // where the run's owner reads it afterwards rather than losing it silently.
    auto recordNativeCallFailure(
        TaskContext& context,
        NativeCallIdentity const& call,
        Error const& error
    ) -> void;

    // Whether this generation is still live, or the failure that spent it.
    //
    // Both front-ends ask this before every primitive, so a caller that swallowed
    // what was raised cannot drive one more engine verb before the generation is
    // torn down. It re-reports under the kind that spent the generation rather than
    // one fixed value: a cancelled run and a run stopped by a framework bug are
    // different verdicts, and reporting either as the other sends a reader looking
    // in the wrong place.
    [[nodiscard]] auto requireLiveGeneration(TaskContext const& context) -> Status;

    // Latches the generation terminal and reports the cancellation when the run's
    // single cancel source has requested a stop.
    //
    // The three time primitives need it and the observation and action primitives do
    // not: those reach the engine, which already fails closed on the same token,
    // while a sleep reaches nothing and would otherwise burn its whole budget on a
    // generation that is already over. It latches BEFORE reporting, which is what
    // makes the next primitive refuse at requireLiveGeneration even if the caller
    // swallowed this.
    //
    // It takes a mutable context because latching that state is the point of the
    // call rather than a side effect of a query.
    [[nodiscard]] auto requireNotCancelled(TaskContext& context) -> Status;
}
