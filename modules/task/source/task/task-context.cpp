#include <task/task-context.hpp>

#include <task/trace.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <memory>
#include <optional>
#include <utility>

namespace uf::task
{
    TaskContext::TaskContext(
        engine::EngineSession session,
        TaskContextConfig config,
        std::unique_ptr<TaskTraceSink> traceSink
    ) noexcept
        : m_session{std::move(session)}
        , m_config{std::move(config)}
        , m_rng{m_config.randomSeed}
        , m_traceSink{std::move(traceSink)}
    {
    }

    // D6 known-popup sweep -- landing note (deliberately not a live hook).
    //
    // capture() and waitForPage() are the task-side observation-cycle boundaries,
    // so a known-popup sweep that runs "once per observation cycle" would attach
    // here. It is left as this note rather than a stored callback for two reasons.
    // First, there is nothing to sweep yet: the real sweep needs the concrete
    // per-daily popup list, which is P0-C work; a nullable no-op callback with no
    // caller and no list would be speculative generality (an untested branch and a
    // stored-callback lifetime surface for zero present benefit). Second, and
    // decisively, a task-side hook is positioned wrong: the tight poll loop that
    // re-captures while waiting for a page lives inside engine::EngineSession::
    // waitForPage, which already calls its own sweepKnownPopups() once per inner
    // cycle (session.cpp). A hook here would fire only at the START of a
    // task-level wait, never on those inner polls, so it could not replicate the
    // per-cycle sweep. The architecturally correct home for the real sweep is the
    // engine's existing no-op seam (D6 replaces it with the bot:on registry in
    // P1); this note records that the task side owns no part of that mechanism.
    auto TaskContext::capture() -> Result<ObservationSeq>
    {
        UF_TRY_VALUE(observation, m_session.observe());

        auto const seq = m_nextSeq;
        ++m_nextSeq;
        m_observations.try_emplace(seq, std::move(observation));
        return seq;
    }

    auto TaskContext::resolvePage(ObservationSeq seq) -> Result<annotation::PageOutcome>
    {
        auto const it = m_observations.find(seq);
        if (it == m_observations.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the observation for this frame was already consumed"
            );
        }

        return it->second.resolvePage();
    }

    auto TaskContext::findAction(
        ObservationSeq seq,
        annotation::RecognizerId recognizerId
    ) -> Result<std::optional<engine::ActionFound>>
    {
        auto const it = m_observations.find(seq);
        if (it == m_observations.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the observation for this frame was already consumed"
            );
        }

        return it->second.findAction(recognizerId);
    }

    auto TaskContext::click(
        ObservationSeq pageSeq,
        ObservationSeq hitSeq,
        annotation::ResolvedPage const& page,
        engine::ActionFound const& action
    ) -> Result<engine::ActReceipt>
    {
        if (pageSeq != hitSeq)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "click received a page and a target from different observations"
            );
        }

        auto const it = m_observations.find(pageSeq);
        if (it == m_observations.end())
        {
            return fail(
                AutomationErrorKind::StaleObservation,
                "the observation for this frame was already consumed"
            );
        }

        // act consumes the observation by rvalue, so the frame is spent whatever
        // the outcome: extract it first and drop the map entry, so any later use
        // of this frame fails StaleObservation above.
        auto observation = std::move(it->second);
        m_observations.erase(it);
        return m_session.act(std::move(observation), page, action);
    }

    auto TaskContext::waitForPage(
        annotation::PageId pageId,
        std::optional<MonotonicInstant::Duration> timeout,
        std::optional<MonotonicInstant::Duration> pollInterval
    ) -> Result<WaitResolved>
    {
        // The per-poll observation cycle is inside engine::EngineSession::
        // waitForPage, which runs its own sweepKnownPopups() each iteration; the
        // task side adds no sweep hook here. See the note above capture() for why
        // the D6 sweep stays on the engine seam rather than a task-side callback.
        UF_TRY_VALUE(
            wait,
            m_session.waitForPage(
                pageId,
                timeout.value_or(m_config.defaultWaitTimeout),
                pollInterval.value_or(m_config.defaultWaitPollInterval)
            )
        );

        // The wait's observation joins the same retained-observation map a capture
        // uses, under a fresh sequence, so the paired frame and page returned to
        // the script are consumed and cross-checked exactly like a captured frame.
        auto const seq = m_nextSeq;
        ++m_nextSeq;
        m_observations.try_emplace(seq, std::move(wait.observation));
        return WaitResolved{.seq = seq, .page = std::move(wait.page)};
    }

    void TaskContext::release(ObservationSeq seq) noexcept
    {
        // Erase by key: a seq already consumed by click, or released by an
        // earlier collection of the same frame handle, is simply absent, so this
        // is a no-op rather than a double-erase.
        m_observations.erase(seq);
    }

    auto TaskContext::liveObservationCount() const noexcept -> uint64
    {
        // size_t is never wider than uint64 on any supported platform, so this
        // widening conversion cannot lose a count.
        return static_cast<uint64>(m_observations.size());
    }

    auto TaskContext::maxLiveObservations() const noexcept -> uint64
    {
        return m_config.maxLiveObservations;
    }

    auto TaskContext::cancelled() const noexcept -> bool
    {
        return m_config.cancellation.stop_requested();
    }

    void TaskContext::markFatal() noexcept
    {
        m_fatal = true;
    }

    auto TaskContext::fatal() const noexcept -> bool
    {
        return m_fatal;
    }

    void TaskContext::latchTraceFailure() noexcept
    {
        m_traceFailed = true;
    }

    auto TaskContext::traceFailed() const noexcept -> bool
    {
        return m_traceFailed;
    }

    auto TaskContext::emitTrace(TaskTraceEvent const& event) -> Status
    {
        if (m_traceSink == nullptr)
        {
            return ok();
        }
        return m_traceSink->emit(event);
    }

    auto TaskContext::nowMillis() noexcept -> int64
    {
        return m_clock.readMillis();
    }

    auto TaskContext::nextRandomUnitDouble() noexcept -> double
    {
        return m_rng.nextUnitDouble();
    }

    auto TaskContext::nextRandomInRange(
        int64 lowInclusive,
        int64 highInclusive
    ) noexcept -> int64
    {
        // Precondition, guaranteed by the binding that reads the script arguments:
        // lowInclusive <= highInclusive and both within +/-2^53. Under it the span
        // fits in uint64, the offset is far below 2^63 so the cast back to int64 is
        // exact, and low + offset stays within [low, high] with no signed overflow.
        UF_ASSERT(lowInclusive <= highInclusive);
        auto const span   = static_cast<uint64>(highInclusive - lowInclusive) + uint64{1};
        auto const offset = m_rng.boundedUint64(span);
        return lowInclusive + static_cast<int64>(offset);
    }
}
