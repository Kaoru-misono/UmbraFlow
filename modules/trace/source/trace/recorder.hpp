#pragma once

#include "event.hpp"
#include "sink.hpp"
#include "stream-validator.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <memory>

namespace uf::trace
{
    // A recorder is confined to one calling thread and one immutable session
    // identity. Its sink is invoked synchronously, so recorder order is also
    // sink-observed append order.
    class TraceRecorder final
    {
        std::unique_ptr<ITraceSink> m_sink;
        TraceStreamSpec             m_stream;
        TraceStreamValidator        m_validator{};
        uint64                      m_nextSequence{1};
        bool                        m_faulted{false};
        bool                        m_exhausted{false};

        TraceRecorder(
            std::unique_ptr<ITraceSink> sink,
            TraceStreamSpec stream
        ) noexcept;

    public:
        [[nodiscard]]
        static auto create(
            std::unique_ptr<ITraceSink> sink,
            TraceStreamSpec const& spec
        ) -> Result<TraceRecorder>;

        TraceRecorder(TraceRecorder const&) = delete;
        TraceRecorder(TraceRecorder&&) noexcept = default;
        auto operator=(TraceRecorder const&) -> TraceRecorder& = delete;
        auto operator=(TraceRecorder&&) noexcept -> TraceRecorder& = default;

        ~TraceRecorder() = default;

        // Invalid input does not consume a sequence. Once append starts, any
        // sink failure permanently faults this recorder because the sink may
        // have persisted an unknown prefix of that event.
        [[nodiscard]] auto emit(TraceEventSpec const& spec) -> Status;
    };
}
