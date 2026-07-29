#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <annotation/recognition.hpp>

#include <engine/session.hpp>

#include <optional>

namespace uf::task
{
    // All a script ever holds of an open observation cycle: a ticket, never the
    // frame. The frame stays in the host's ledger, so the moment its several
    // megabytes are released is a host decision rather than whatever the Lua
    // collector decides to do next.
    //
    // `generation` names the ledger that minted the ticket and `ordinal` names
    // the cycle within it. Both are compared on every use, so a ticket left over
    // from a spent VM generation is rejected instead of colliding with a live
    // ordinal in the next one.
    struct CycleTicket final
    {
        uint64 generation{};
        uint64 ordinal{};
    };

    // The host-side ledger of open observation cycles. It holds AT MOST ONE.
    //
    // That ceiling is the whole reason this type exists. While two cycles could
    // be open at once, "the page and the hit came from the same frame" stays a
    // comparison the host performs, and the host must not depend on the Luau
    // framework's discipline for a guarantee. With one entry there is no second
    // frame: mixing two is not rejected, it cannot be expressed. The
    // std::optional below IS that ceiling, which is why nothing here counts live
    // observations and no configurable bound exists to be set wrong.
    //
    // Nothing needs two: the framework's wait loop opens and closes one per
    // poll, an interrupt handler is handed the current cycle, and a handler that
    // consumes the observation lets the outer loop reopen.
    //
    // NOT thread-safe: every method runs on the VM's owning thread. Destroying
    // the ledger releases whatever cycle is still open, which is the host's
    // backstop when a generation is torn down mid-script.
    class CycleLedger final
    {
        // The one open cycle: the frame it retains and, once it has been
        // resolved, the page that is this cycle's click authorization evidence.
        // `page` stays empty until the cycle resolves one, and a click with no
        // page is refused -- that emptiness is what makes unauthorized delivery
        // impossible rather than merely checked.
        struct OpenCycle final
        {
            engine::Observation                     observation;
            uint64                                  ordinal{};
            std::optional<annotation::ResolvedPage> page{};
        };

        uint64                   m_generation;
        uint64                   m_nextOrdinal{1};
        std::optional<OpenCycle> m_open{};

        // True when `ticket` names the cycle this ledger currently holds open:
        // same generation stamp, same ordinal. Every ticket-taking operation
        // routes through it, so "is this the open cycle" has one answer.
        [[nodiscard]]
        auto namesOpenCycle(CycleTicket ticket) const noexcept -> bool;

    public:
        // What consume() hands back: the frame moved out of the ledger and the
        // page that cycle resolved. Both are required to authorize a click and
        // neither can be supplied by a script, which is the point.
        struct Consumed final
        {
            engine::Observation      observation;
            annotation::ResolvedPage page;
        };

        // Mints this ledger's generation stamp. One ledger lives and dies with
        // one VM generation, so a fresh stamp per instance is what makes a
        // ticket from a spent generation rejectable instead of accidentally
        // valid in the next one.
        CycleLedger() noexcept;

        [[nodiscard]] auto isOpen() const noexcept -> bool;

        // The one-cycle rule, stated once. Opening a second cycle is a framework
        // bug rather than a script error -- the framework opens and closes one
        // cycle per poll and hands an interrupt handler the current one -- so it
        // is InternalInvariant, not a Tier B failure a script can catch and
        // retry.
        [[nodiscard]] auto requireClosed() const -> Status;

        // Fails StaleObservation unless `ticket` names the open cycle. Under the
        // one-cycle rule this is the entire staleness check: a ticket that is
        // not the open cycle's names a cycle that no longer exists, and there is
        // no second cycle for it to have come from.
        [[nodiscard]] auto requireOpen(CycleTicket ticket) const -> Status;

        // The same check for the ordinal a hit handle carries. It is a separate
        // entry point only so the failure names the hit rather than the ticket;
        // both resolve against the one open cycle.
        [[nodiscard]] auto requireOpenOrdinal(uint64 ordinal) const -> Status;

        // Opens the cycle over `observation` and returns the ticket that names
        // it. Precondition: requireClosed() has just succeeded. It is a separate
        // call so the caller can refuse before spending a whole capture on a
        // frame this ledger could not hold.
        [[nodiscard]] auto open(engine::Observation observation) -> CycleTicket;

        // Releases the frame the cycle `ticket` names retains, reporting whether
        // there was one to release. A ticket that names no open cycle -- closed
        // already, consumed by a click, or minted by another generation -- is a
        // harmless no-op, so a framework cleanup path can close unconditionally.
        [[nodiscard]] auto close(CycleTicket ticket) noexcept -> bool;

        // The frame the open cycle retains. Precondition: requireOpen has just
        // succeeded for the ticket in hand. The borrow lasts until the next
        // mutating call on this ledger.
        [[nodiscard]]
        auto observation() const noexcept UF_LIFETIME_BOUND
            -> engine::Observation const&;

        // Records `page` as the open cycle's click authorization evidence. Same
        // precondition as observation(): the caller has already validated its
        // ticket against this ledger.
        auto rememberPage(annotation::ResolvedPage page) -> void;

        // Spends the cycle `ticket` names: the frame leaves the ledger and the
        // ticket dies here, before the click that follows can do anything with
        // the frame. Fails StaleObservation for a ticket that names no open
        // cycle, and ActionRejected when the cycle resolved no page, because
        // then the click has no authorization evidence and no script can supply
        // any.
        [[nodiscard]] auto consume(CycleTicket ticket) -> Result<Consumed>;
    };
}
