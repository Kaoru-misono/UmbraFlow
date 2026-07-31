#include <task/task-context.hpp>

#include <task/cycle-ledger.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/time/poll-sleep.hpp>
#include <core/types/integer.hpp>

#include <annotation/resource.hpp>
#include <annotation/recognition.hpp>

#include <domain/error.hpp>
#include <domain/key.hpp>

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

    // D6 known-popup sweep -- landing note (deliberately not a hook here).
    //
    // The sweep has a home now, and it is neither this file nor the engine: it
    // is the framework's interrupt registry. A task declares the popups it knows
    // through task.interrupt, and the Luau wait loop offers every cycle it opens
    // to that registry before testing the target page. That is the per-cycle
    // sweep the design asked for, and it fires on the polls that matter -- the
    // ones in the middle of a long wait -- which is exactly what neither
    // candidate C++ position could do. A task-side callback would have fired
    // once at the start of a wait; the engine's own seam sat inside a poll loop
    // the registry could not reach.
    //
    // What stays true: the task side owns no part of that mechanism. This note
    // records where it went so the absence is not read as an omission.
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
        annotation::ElementId elementId
    ) -> Result<std::optional<engine::ActionFound>>
    {
        UF_TRY(m_cycles.requireOpen(ticket));

        // Locating an element is page-scoped: the reference row carries the
        // search region this page refines and the appearance it pins, and a
        // page that does not exercise interact on the element has no action
        // there to find. So the cycle's own resolved page is what the search
        // runs against -- the same page a click on the result would be
        // authorized by, and one no script can substitute.
        //
        // A cycle that resolved none has nothing to search against. Refusing
        // here rather than reporting an absence keeps "the search looked and
        // found nothing" distinct from "the search could not run".
        //
        // The message names the missing STEP rather than a refused action,
        // because that is what happened: nothing was attempted here. The kind
        // says the same thing, which is why it is PageUnresolved and not
        // ActionRejected -- the latter is reserved for a page that resolved and
        // then did not authorise the element, which is a different repair.
        auto const pageId = m_cycles.resolvedPageId();
        if (!pageId)
        {
            return fail(
                AutomationErrorKind::PageUnresolved,
                "this observation cycle has not resolved a page, and finding an "
                "element is page-scoped: the refined search region, the pinned "
                "appearance and the interact authorisation all live on the "
                "page's reference to the element, so a find has nothing to work "
                "from until a page resolves. Resolve this cycle's page first, "
                "then find"
            );
        }
        return m_session.findAction(m_cycles.observation(), *pageId, elementId);
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

    auto TaskContext::cycleKey(CycleTicket ticket, KeyName key) -> Status
    {
        // spend rather than consume: a keystroke names no coordinate, so there is
        // no page for it to be authorized against (see the header). The ticket is
        // still checked against the one open cycle first, so a stale ticket leaves
        // the cycle open for the framework to close rather than destroying a frame
        // a live ticket still names.
        UF_TRY_VALUE(observation, m_cycles.spend(ticket));

        // pressKey consumes the frame by rvalue, so the cycle is spent whatever the
        // outcome; spend already dropped the ledger entry, which is what makes
        // every later use of this ticket fail StaleObservation.
        UF_TRY(m_session.pressKey(std::move(observation), key));
        return ok();
    }

    auto TaskContext::waitUntil(
        MonotonicInstant deadline,
        MonotonicInstant::Duration interval
    ) const -> bool
    {
        // Report the expiry without sleeping when the deadline has already
        // passed: a wait whose budget is spent must not first spend an interval.
        if (MonotonicInstant::now() >= deadline)
        {
            return false;
        }

        pollSleep(interval, deadline, m_config.cancellation);
        return MonotonicInstant::now() < deadline;
    }

    auto TaskContext::settle(MonotonicInstant::Duration duration) const -> void
    {
        // The settle's own end is its deadline, so the shared poll sleep bounds
        // it by the same instant its interval would reach and no separate
        // ceiling has to be reconciled with it.
        auto const until = MonotonicInstant::now().checkedAdd(duration);
        if (!until)
        {
            // Unreachable while the binding enforces k_maxSettleDuration, which
            // is far below any monotonic overflow. Sleeping for nothing is the
            // fail-closed answer: a pause the host cannot bound is one it must
            // not take.
            return;
        }

        pollSleep(duration, *until, m_config.cancellation);
    }

    auto TaskContext::cancellationRequested() const noexcept -> bool
    {
        return m_config.cancellation.stop_requested();
    }

    auto TaskContext::hasOpenCycle() const noexcept -> bool
    {
        return m_cycles.isOpen();
    }

    void TaskContext::markTerminal(AutomationErrorKind kind) noexcept
    {
        if (!m_terminal.has_value())
        {
            m_terminal = kind;
        }
    }

    auto TaskContext::fatal() const noexcept -> bool
    {
        return m_terminal.has_value();
    }

    auto TaskContext::terminalKind() const noexcept -> std::optional<AutomationErrorKind>
    {
        return m_terminal;
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
