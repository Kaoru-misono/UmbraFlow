#pragma once

#include "event.hpp"
#include "sink.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>

#include <memory>

namespace uf::trace
{
    // The single writer of one run's evidence stream. It owns the sink and holds
    // the run identity every event must carry: a monotonic sequence, the run id
    // and the generation id. emit() stamps those onto the event, adds the wall
    // clock, and forwards; nothing else can reach the sink, so no emitter can
    // forget the stamp.
    //
    // One recorder per run. It is non-copyable and non-movable because engine and
    // task store a borrow of it (see their lifetime contracts), so its address
    // must stay stable for the whole run.
    class TraceRecorder final
    {
        std::unique_ptr<ITraceSink> m_sink;
        TaskRunId                   m_runId;
        GenerationId                m_generationId;
        uint64                      m_nextSequence{1};

    public:
        // `sink` must not be null: a run either records its evidence or is
        // configured with a sink that deliberately discards it, and a null sink
        // would make "tracing is off" an untyped third state every emitter would
        // have to reason about.
        TraceRecorder(
            std::unique_ptr<ITraceSink> sink,
            TaskRunId runId,
            GenerationId generationId
        ) noexcept;

        TraceRecorder(TraceRecorder const&) = delete;
        TraceRecorder(TraceRecorder&&) = delete;
        auto operator=(TraceRecorder const&) -> TraceRecorder& = delete;
        auto operator=(TraceRecorder&&) -> TraceRecorder& = delete;

        ~TraceRecorder() = default;

        // Stamps `event` with the next sequence number, the run identity and the
        // current wall clock, then forwards it to the sink. The sequence advances
        // whether or not the sink succeeds, so a gap in the stream is visible as
        // a missing number rather than silently closed up.
        [[nodiscard]] auto emit(TraceEvent const& event) -> Status;

        [[nodiscard]] auto runId() const noexcept -> TaskRunId;
        [[nodiscard]] auto generationId() const noexcept -> GenerationId;
    };
}
