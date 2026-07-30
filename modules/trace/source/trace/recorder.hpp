#pragma once

#include "event.hpp"
#include "sink.hpp"
#include "stream-validator.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>

#include <memory>

namespace uf::trace
{
    // The single writer of one run's evidence stream. It owns the sink and holds
    // the run identity every event must carry: a monotonic sequence, the run id
    // and the generation id. emit() validates the event against the stream
    // protocol, stamps those onto it, adds the wall clock and the open step
    // scope, and forwards; nothing else can reach the sink, so no emitter can
    // forget the stamp or go around the protocol.
    //
    // The validator lives here for the same structural reason the sequence
    // counter does. Its rules span the whole stream -- the run bracket the host
    // writes, the native calls the binding writes, the semantic events the Luau
    // framework requests -- and this is the one object that sees all three.
    //
    // One recorder per run. It is non-copyable and non-movable because engine and
    // task store a borrow of it (see their lifetime contracts), so its address
    // must stay stable for the whole run.
    class TraceRecorder final
    {
        std::unique_ptr<ITraceSink> m_sink;
        TraceStreamValidator        m_validator;
        TaskRunId                   m_runId;
        GenerationId                m_generationId;
        FrontEnd                    m_frontEnd;
        uint64                      m_nextSequence{1};

    public:
        // `sink` must not be null: a run either records its evidence or is
        // configured with a sink that deliberately discards it, and a null sink
        // would make "tracing is off" an untyped third state every emitter would
        // have to reason about.
        //
        // `frontEnd` has no default. Which front-end drove a run is not something
        // a construction site may leave unsaid -- a defaulted attribution would
        // silently name one of them on every stream that forgot to choose -- and
        // the value belongs to the caller that already latched it (see
        // trace::FrontEnd). It reaches both the stamp and the validator, so the
        // rules that depend on it and the attribution a reader sees come from one
        // value.
        TraceRecorder(
            std::unique_ptr<ITraceSink> sink,
            TaskRunId runId,
            GenerationId generationId,
            FrontEnd frontEnd
        ) noexcept;

        TraceRecorder(TraceRecorder const&) = delete;
        TraceRecorder(TraceRecorder&&) = delete;
        auto operator=(TraceRecorder const&) -> TraceRecorder& = delete;
        auto operator=(TraceRecorder&&) -> TraceRecorder& = delete;

        ~TraceRecorder() = default;

        // Admits `event` into the stream, then stamps it with the next sequence
        // number, the run identity, the current wall clock and the framework step
        // scope open at this instant, and forwards it to the sink.
        //
        // The sequence advances whether or not the SINK succeeds, so a gap in the
        // stream is visible as a missing number rather than silently closed up.
        // It does NOT advance for an event the validator refused: that event
        // never entered the stream, so a number spent on it would report a line
        // that was never written. The refusal's own error carries which rule
        // broke and whether it was a request the stream declines (InvalidResource)
        // or a protocol breach (InternalInvariant); see stream-validator.hpp.
        [[nodiscard]] auto emit(TraceEvent const& event) -> Status;

        // Whether the framework left a step or an interrupt match open. The run
        // owner asks before writing run.finished, so an unclosed scope becomes
        // the run's reported failure instead of a bracket that quietly closed
        // over a framework bug.
        [[nodiscard]] auto requireScopesClosed() const -> Status;

        [[nodiscard]] auto runId() const noexcept -> TaskRunId;
        [[nodiscard]] auto generationId() const noexcept -> GenerationId;
        [[nodiscard]] auto frontEnd() const noexcept -> FrontEnd;
    };
}
