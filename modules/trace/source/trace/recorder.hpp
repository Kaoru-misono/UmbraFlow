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
    // The single writer of one run's evidence stream. It owns the sink and the run
    // identity every event must carry -- a monotonic sequence, the run id and the
    // generation id -- and emit() validates against the stream protocol, stamps,
    // adds the wall clock and the open step scope, and forwards; nothing else can
    // reach the sink, so no emitter can forget the stamp or go around the protocol.
    // The validator lives here for the reason the sequence counter does: its rules
    // span the whole stream and this is the one object that sees all of it. One
    // recorder per run, non-copyable and non-movable because engine and task store
    // a borrow of it, so its address must stay stable for the whole run.
    class TraceRecorder final
    {
        std::unique_ptr<ITraceSink> m_sink;
        TraceStreamValidator        m_validator;
        TaskRunId                   m_runId;
        GenerationId                m_generationId;
        FrontEnd                    m_frontEnd;
        uint64                      m_nextSequence{1};

    public:
        // `sink` must not be null: a run either records its evidence or is given a
        // sink that deliberately discards it, and a null one would make "tracing is
        // off" an untyped third state every emitter must reason about. `frontEnd`
        // has no default because a defaulted attribution would silently name one on
        // every stream that forgot to choose; it reaches both the stamp and the
        // validator, so the rules that depend on it and the attribution a reader
        // sees come from one value.
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

        // Admits `event` into the stream, stamps it with the next sequence number,
        // the run identity, the wall clock and the step scope open at this instant,
        // and forwards it to the sink. The sequence advances whether or not the SINK
        // succeeds, so a lost line shows as a missing number; it does NOT advance
        // for an event the validator refused, which never entered the stream.
        [[nodiscard]] auto emit(TraceEvent const& event) -> Status;

        // Whether the framework left a step or an interrupt match open. The run
        // owner asks before writing run.finished (see stream-validator.hpp).
        [[nodiscard]] auto requireScopesClosed() const -> Status;

        [[nodiscard]] auto runId() const noexcept -> TaskRunId;
        [[nodiscard]] auto generationId() const noexcept -> GenerationId;
        [[nodiscard]] auto frontEnd() const noexcept -> FrontEnd;
    };
}
