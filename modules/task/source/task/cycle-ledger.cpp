#include <task/cycle-ledger.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <atomic>
#include <limits>
#include <optional>
#include <utility>

namespace uf::task
{
    namespace
    {
        // The process-wide source of ledger generation stamps. Every ledger
        // takes one at construction, so two ledgers never share a stamp and a
        // ticket is only ever valid in the ledger that minted it. Atomic because
        // two hosts on two threads may each build a generation, even though one
        // ledger is confined to its own VM's thread afterwards.
        [[nodiscard]]
        auto mintGeneration() noexcept -> uint64
        {
            static auto s_next = std::atomic<uint64>{1};
            auto const  stamp  = s_next.fetch_add(uint64{1}, std::memory_order_relaxed);

            // A wrapped stamp would hand a ticket from a spent generation back
            // its validity, which is the exact hazard the stamp closes, so
            // exhaustion stops the process instead of silently reusing one.
            // Reaching it takes 2^64 VM generations in a single process.
            UF_CHECK(stamp != std::numeric_limits<uint64>::max());
            return stamp;
        }
    }

    CycleLedger::CycleLedger() noexcept
        : m_generation{mintGeneration()}
    {
    }

    auto CycleLedger::namesOpenCycle(CycleTicket ticket) const noexcept -> bool
    {
        return (
            m_open.has_value()
            && ticket.generation == m_generation
            && ticket.ordinal == m_open->ordinal
        );
    }

    auto CycleLedger::isOpen() const noexcept -> bool
    {
        return m_open.has_value();
    }

    auto CycleLedger::requireClosed() const -> Status
    {
        if (!m_open.has_value())
        {
            return ok();
        }
        return fail(
            AutomationErrorKind::InternalInvariant,
            "an observation cycle is already open; a generation holds at most "
            "one, so close or consume the open cycle before opening another"
        );
    }

    auto CycleLedger::requireOpen(CycleTicket ticket) const -> Status
    {
        if (namesOpenCycle(ticket))
        {
            return ok();
        }
        return fail(
            AutomationErrorKind::StaleObservation,
            "this observation cycle is no longer open; its frame was released "
            "or already consumed by a delivered input"
        );
    }

    auto CycleLedger::requireOpenOrdinal(uint64 ordinal) const -> Status
    {
        if (m_open.has_value() && m_open->ordinal == ordinal)
        {
            return ok();
        }
        return fail(
            AutomationErrorKind::StaleObservation,
            "this hit came from an observation cycle that is no longer open"
        );
    }

    auto CycleLedger::open(engine::Observation observation) -> CycleTicket
    {
        // Established by requireClosed at the only call site. Overwriting an
        // open cycle here would drop a frame a live ticket still names.
        UF_ASSERT(!m_open.has_value());

        auto const ordinal = m_nextOrdinal;
        ++m_nextOrdinal;
        m_open = OpenCycle{
            .observation = std::move(observation),
            .ordinal     = ordinal,
        };
        return CycleTicket{.generation = m_generation, .ordinal = ordinal};
    }

    auto CycleLedger::close(CycleTicket ticket) noexcept -> bool
    {
        if (!namesOpenCycle(ticket))
        {
            return false;
        }
        m_open.reset();
        return true;
    }

    auto CycleLedger::observation() const noexcept -> engine::Observation const&
    {
        UF_ASSERT(m_open.has_value());
        return m_open->observation;
    }

    auto CycleLedger::rememberPage(annotation::ResolvedPage page) -> void
    {
        UF_ASSERT(m_open.has_value());
        m_open->page = std::move(page);
    }

    auto CycleLedger::resolvedPageId() const noexcept -> std::optional<annotation::PageId>
    {
        UF_ASSERT(m_open.has_value());
        if (!m_open->page.has_value())
        {
            return std::nullopt;
        }
        return m_open->page->pageId();
    }

    auto CycleLedger::consume(CycleTicket ticket) -> Result<Consumed>
    {
        UF_TRY(requireOpen(ticket));

        if (!m_open->page.has_value())
        {
            // The same skipped step the find refusal names, one verb later, so
            // it carries the same kind: nothing was judged and refused here
            // either -- the click never reached the authorization check that
            // could have rejected it.
            return fail(
                AutomationErrorKind::PageUnresolved,
                "this observation cycle has not resolved a page, and the page "
                "IS a click's authorization evidence -- authorisation is the "
                "resolved page's reference to the element, and no script can "
                "supply one. Resolve this cycle's page first, then find and "
                "click"
            );
        }

        // The click consumes the frame by rvalue whatever it then does with it,
        // so the cycle is spent the moment it is handed over: take both parts
        // out and drop the entry here, which is what kills the ticket.
        auto consumed = Consumed{
            .observation = std::move(m_open->observation),
            .page        = *std::move(m_open->page),
        };
        m_open.reset();
        return consumed;
    }

    auto CycleLedger::spend(CycleTicket ticket) -> Result<engine::Observation>
    {
        UF_TRY(requireOpen(ticket));

        auto observation = std::move(m_open->observation);
        m_open.reset();
        return observation;
    }
}
