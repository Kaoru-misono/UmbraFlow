#pragma once

#include "event.hpp"

#include <core/error/result.hpp>

namespace uf::trace
{
    // A port that records one stamped trace event. Traceability is a load-bearing
    // constraint, so an emit failure is an error rather than a best-effort side
    // effect: an emitter writes at the decision instant, before any caller can
    // swallow the failure it describes, and treats a failed emit as aborting the
    // operation whose evidence was lost.
    //
    // The parameter is a StampedTraceEvent rather than a TraceEvent, so a sink can
    // only ever be reached through the TraceRecorder that stamps the sequence, run
    // id and generation id. The invariant is on construction, not on ownership:
    // StampedTraceEvent's constructor is private to TraceRecorder, so no code
    // outside modules/trace can mint one, while any sink may keep the events it is
    // handed -- a recording test sink owns a vector of them.
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
