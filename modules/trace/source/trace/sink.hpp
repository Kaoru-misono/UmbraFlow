#pragma once

#include "event.hpp"

#include <core/error/result.hpp>

namespace uf::trace
{
    // A port that records one stamped trace event. Traceability is load-bearing, so
    // an emit failure is an error rather than a best-effort side effect: an emitter
    // writes at the decision instant and treats a failed emit as aborting the
    // operation whose evidence was lost. StampedTraceEvent rather than TraceEvent
    // because its constructor is private to TraceRecorder: a sink is reachable only
    // through the recorder that stamps the sequence and run identity, while any
    // sink may keep the events it is handed.
    class ITraceSink
    {
    public:
        ITraceSink() = default;

        ITraceSink(ITraceSink const&) = delete;
        ITraceSink(ITraceSink&&) = delete;
        auto operator=(ITraceSink const&) -> ITraceSink& = delete;
        auto operator=(ITraceSink&&) -> ITraceSink& = delete;

        virtual ~ITraceSink() = default;

        [[nodiscard]] virtual auto emit(StampedTraceEvent const& event) -> Status = 0;
    };
}
