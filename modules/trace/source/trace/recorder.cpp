#include "recorder.hpp"

#include "event.hpp"
#include "stream-validator.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/ids.hpp>

#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace uf::trace
{
    namespace
    {
        // Milliseconds since the Unix epoch. The one field a trace may not
        // reproduce, which is why it lives in the non-golden `meta` member.
        [[nodiscard]]
        auto wallClockUnixMillis() noexcept -> int64
        {
            using MillisecondRep = std::chrono::milliseconds::rep;
            static_assert(std::is_signed_v<MillisecondRep>);
            static_assert(sizeof(MillisecondRep) <= sizeof(int64));

            auto const sinceEpoch = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            );
            return static_cast<int64>(sinceEpoch.count());
        }
    }

    TraceRecorder::TraceRecorder(
        std::unique_ptr<ITraceSink> sink,
        TaskRunId runId,
        GenerationId generationId,
        FrontEnd frontEnd
    ) noexcept
        : m_sink{std::move(sink)}
        , m_validator{frontEnd}
        , m_runId{runId}
        , m_generationId{generationId}
        , m_frontEnd{frontEnd}
    {
        UF_CHECK_MSG(m_sink != nullptr, "a trace recorder requires a sink");
    }

    auto TraceRecorder::emit(TraceEvent const& event) -> Status
    {
        UF_TRY_VALUE(openSteps, m_validator.admit(event));

        auto const sequence = m_nextSequence;
        ++m_nextSequence;

        auto const stamped = StampedTraceEvent{
            event,
            std::move(openSteps),
            sequence,
            m_runId,
            m_generationId,
            m_frontEnd,
            wallClockUnixMillis(),
        };
        return m_sink->emit(stamped);
    }

    auto TraceRecorder::requireScopesClosed() const -> Status
    {
        return m_validator.requireScopesClosed();
    }

    auto TraceRecorder::runId() const noexcept -> TaskRunId { return m_runId; }

    auto TraceRecorder::generationId() const noexcept -> GenerationId
    {
        return m_generationId;
    }

    auto TraceRecorder::frontEnd() const noexcept -> FrontEnd { return m_frontEnd; }
}
