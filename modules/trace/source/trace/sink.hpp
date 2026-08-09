#pragma once

#include "event.hpp"

#include <core/error/result.hpp>

namespace uf::trace
{
    // One synchronous append boundary. Implementations observe calls in order
    // and never alter an event accepted by an earlier call. Success means this
    // sink accepted the complete event; durability remains a sink-specific
    // guarantee. A failure may have appended an unknown prefix, so the owning
    // recorder becomes permanently faulted and will not emit a later event.
    class ITraceSink
    {
    public:
        ITraceSink() = default;

        ITraceSink(ITraceSink const&) = delete;
        ITraceSink(ITraceSink&&) = delete;
        auto operator=(ITraceSink const&) -> ITraceSink& = delete;
        auto operator=(ITraceSink&&) -> ITraceSink& = delete;

        virtual ~ITraceSink() = default;

        [[nodiscard]] virtual auto append(TraceEvent const& event) -> Status = 0;
    };
}
