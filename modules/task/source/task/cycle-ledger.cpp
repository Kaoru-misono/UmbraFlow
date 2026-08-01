#include <task/cycle-ledger.hpp>

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <engine/session.hpp>

#include <atomic>
#include <limits>
#include <optional>
#include <utility>

namespace uf::task
{
    // Atomic because two hosts on two threads may each build a generation, even
    // though one store is confined to its own VM's thread afterwards.
    auto mintHandleGeneration() noexcept -> uint64
    {
        static auto s_next = std::atomic<uint64>{1};
        auto const  stamp  = s_next.fetch_add(uint64{1}, std::memory_order_relaxed);

        // A wrapped stamp would hand a handle from a spent generation back its
        // validity, which is the exact hazard the stamp closes, so exhaustion
        // stops the process instead of silently reusing one. Reaching it takes
        // 2^64 VM generations in a single process.
        UF_CHECK(stamp != std::numeric_limits<uint64>::max());
        return stamp;
    }

    CycleLedger::CycleLedger() noexcept
        : m_generation{mintHandleGeneration()}
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

    auto CycleLedger::readsCharged() const noexcept -> uint32
    {
        UF_ASSERT(m_open.has_value());
        return m_open->reads;
    }

    auto CycleLedger::chargeRead() noexcept -> void
    {
        UF_ASSERT(m_open.has_value());
        ++m_open->reads;
    }

    auto CycleLedger::spend(CycleTicket ticket) -> Result<engine::Observation>
    {
        UF_TRY(requireOpen(ticket));

        // The delivery consumes the frame by rvalue whatever it then does with
        // it, so the cycle is spent the moment it is handed over: take the frame
        // out and drop the entry here, which is what kills the ticket.
        auto observation = std::move(m_open->observation);
        m_open.reset();
        return observation;
    }
}
