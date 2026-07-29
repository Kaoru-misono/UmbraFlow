#include <task/task-context.hpp>

#include <task/cycle-ledger.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>
#include <annotation/recognition.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <trace/event.hpp>
#include <trace/recorder.hpp>

#include <optional>
#include <utility>
#include <variant>

namespace uf::task
{
    TaskContext::TaskContext(
        engine::EngineSession session,
        trace::TraceRecorder& recorder,
        TaskContextConfig config
    ) noexcept
        : m_session{std::move(session)}
        , m_config{std::move(config)}
        , m_rng{m_config.randomSeed}
        , m_recorder{recorder}
    {
    }

    // D6 known-popup sweep -- landing note (deliberately not a live hook).
    //
    // openCycle() and waitForPage() are the task-side observation-cycle
    // boundaries, so a known-popup sweep that runs "once per observation cycle"
    // would attach here. It is left as this note rather than a stored callback
    // for two reasons. First, there is nothing to sweep yet: the real sweep needs
    // the concrete per-daily popup list, which is P0-C work; a nullable no-op
    // callback with no caller and no list would be speculative generality (an
    // untested branch and a stored-callback lifetime surface for zero present
    // benefit). Second, and decisively, a task-side hook is positioned wrong: the
    // tight poll loop that re-captures while waiting for a page lives inside
    // engine::EngineSession::waitForPage, which already calls its own
    // sweepKnownPopups() once per inner cycle (session.cpp). A hook here would
    // fire only at the START of a task-level wait, never on those inner polls, so
    // it could not replicate the per-cycle sweep. The architecturally correct
    // home for the real sweep is the engine's existing no-op seam (D6 replaces it
    // with the bot:on registry in P1); this note records that the task side owns
    // no part of that mechanism.
    auto TaskContext::openCycle() -> Result<CycleTicket>
    {
        // Refuse before observing. The ledger holds one cycle, and a capture
        // whose frame it could not hold would spend a whole screenshot only to
        // produce an error.
        UF_TRY(m_cycles.requireClosed());
        UF_TRY_VALUE(observation, m_session.observe());
        return m_cycles.open(std::move(observation));
    }

    auto TaskContext::closeCycle(CycleTicket ticket) noexcept -> bool
    {
        return m_cycles.close(ticket);
    }

    auto TaskContext::cyclePage(
        CycleTicket ticket
    ) -> Result<std::optional<annotation::ResolvedPage>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));
        UF_TRY_VALUE(outcome, m_session.resolvePage(m_cycles.observation()));

        auto const* const p_resolved =
            std::get_if<annotation::ResolvedPage>(&outcome);
        if (p_resolved == nullptr)
        {
            // Unknown or Ambiguous: a completed resolution the engine already
            // traced, and the cycle keeps no evidence, so a later click on it
            // has nothing to authorize against.
            return std::nullopt;
        }

        m_cycles.rememberPage(*p_resolved);
        return *p_resolved;
    }

    auto TaskContext::cycleFind(
        CycleTicket ticket,
        annotation::RecognizerId recognizerId
    ) -> Result<std::optional<engine::ActionFound>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));
        return m_session.findAction(m_cycles.observation(), recognizerId);
    }

    auto TaskContext::cycleClick(
        CycleTicket ticket,
        uint64 hitCycleOrdinal,
        engine::ActionFound const& action
    ) -> Result<engine::ActReceipt>
    {
        // Both the ticket and the hit are checked against the one open cycle
        // before it is spent, so a stale hit leaves the cycle open for the
        // framework to close rather than destroying a frame the script still
        // has a live ticket for.
        UF_TRY(m_cycles.requireOpen(ticket));
        UF_TRY(m_cycles.requireOpenOrdinal(hitCycleOrdinal));
        UF_TRY_VALUE(consumed, m_cycles.consume(ticket));

        // act consumes the frame by rvalue, so the cycle is spent whatever the
        // outcome; consume already dropped the ledger entry, which is what makes
        // every later use of this ticket fail StaleObservation.
        return m_session.act(
            std::move(consumed.observation),
            consumed.page,
            action
        );
    }

    auto TaskContext::waitForPage(
        annotation::PageId pageId,
        std::optional<MonotonicInstant::Duration> timeout,
        std::optional<MonotonicInstant::Duration> pollInterval
    ) -> Result<CycleWait>
    {
        // The per-poll observation cycle is inside engine::EngineSession::
        // waitForPage, which runs its own sweepKnownPopups() each iteration; the
        // task side adds no sweep hook here. See the note above openCycle() for
        // why the D6 sweep stays on the engine seam rather than a task-side
        // callback.
        UF_TRY(m_cycles.requireClosed());
        UF_TRY_VALUE(
            wait,
            m_session.waitForPage(
                pageId,
                timeout.value_or(m_config.defaultWaitTimeout),
                pollInterval.value_or(m_config.defaultWaitPollInterval)
            )
        );

        // The wait's observation opens the generation's one cycle exactly as a
        // bare open would, and the page the wait already resolved becomes that
        // cycle's authorization evidence, so a click needs no second resolution.
        auto const ticket = m_cycles.open(std::move(wait.observation));
        m_cycles.rememberPage(wait.page);
        return CycleWait{.ticket = ticket, .page = std::move(wait.page)};
    }

    auto TaskContext::hasOpenCycle() const noexcept -> bool
    {
        return m_cycles.isOpen();
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

    auto TaskContext::emitTrace(trace::TraceEvent const& event) -> Status
    {
        return m_recorder.emit(event);
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
