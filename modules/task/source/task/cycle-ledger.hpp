#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <engine/session.hpp>

#include <optional>

namespace uf::task
{
    // Draws the next process-wide stamp for one family of host-minted handles.
    //
    // Every store that hands a script a name for something the host keeps takes
    // one of these at construction, so a handle carries the identity of the
    // store that minted it and a handle left over from a spent generation is
    // rejected instead of colliding with a live ordinal in the next one. One
    // counter serves all of them: the stamp only has to be unique, and a second
    // counter would be a second thing to reason about for no extra property.
    [[nodiscard]] auto mintHandleGeneration() noexcept -> uint64;

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
        // The one open cycle: the frame it retains, and how much of the cycle's
        // own read budget has been spent on it.
        //
        // It carried a resolved page until the script-owned page model's first
        // retirement wave. That page was the click's authorisation evidence while
        // C++ owned the model; the model is layer two's now, so the evidence is
        // the receipt observe.resolve_page mints against this cycle's ticket
        // (modules/task/runtime/observe.luau) and the ledger has nothing to hold.
        // What the ledger still owns is the part a Luau table could not be
        // trusted with: one frame, spent once.
        struct OpenCycle final
        {
            engine::Observation observation;
            uint64              ordinal{};

            // How many text reads have already been charged to this cycle.
            //
            // It lives on the cycle rather than on the run because that is the
            // scope the budget is stated in: a wait loop that reads once per
            // poll is normal, and a bound spanning the whole run would either
            // stop that loop or be so large it stops nothing.
            uint32 reads{};

            // How many crops have been charged to this cycle, on exactly the
            // same reasoning. A crop is megabytes of pixels encoded into a PNG,
            // so it is its own budget dimension for the reason a read is: the
            // units are not comparable, and one number covering a SAD
            // comparison, a line read and a whole-panel encode describes none of
            // them.
            uint32 crops{};
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

        // How many text reads the open cycle has already spent, and the charge
        // for one more. Same precondition as observation().
        //
        // The count is the ledger's rather than the caller's because the ledger
        // is what a cycle IS: a counter kept beside it would have to be reset by
        // whoever noticed the cycle changed, and nothing would check that it was.
        [[nodiscard]] auto readsCharged() const noexcept -> uint32;

        auto chargeRead() noexcept -> void;

        // The same pair for the crop budget. Same precondition, same reason the
        // count belongs to the ledger rather than to a caller.
        [[nodiscard]] auto cropsCharged() const noexcept -> uint32;

        auto chargeCrop() noexcept -> void;

        // Releases whatever cycle is open, reporting whether there was one.
        //
        // IT IS NOT REACHABLE FROM ANY SCRIPT and must not become so. Every
        // script-facing release names a ticket, which is what makes closing an
        // act about a cycle the caller actually holds. This one names none
        // because its caller holds none: the exploration front-end runs one
        // agent-supplied chunk per queue line, and a chunk that opened a cycle
        // and raised before closing it would otherwise leave the ledger holding
        // a frame that the NEXT chunk's cycle_open reports as a framework bug --
        // spending the whole session over one bad line. Sweeping between lines
        // is the host cleaning up after a bracket it owns, which is the same
        // thing the destructor does when the generation is torn down.
        auto closeOpen() noexcept -> bool;

        // Spends the cycle `ticket` names: the frame leaves the ledger and the
        // ticket dies here, before the input that follows can do anything with
        // the frame. Fails StaleObservation for a ticket that names no open
        // cycle.
        //
        // One entry point serves the click and the keystroke alike. There were
        // two while a click additionally required the page this cycle had
        // resolved; that requirement moved to layer two, so a second spelling
        // would now differ from the first in nothing at all.
        [[nodiscard]] auto spend(CycleTicket ticket) -> Result<engine::Observation>;
    };
}
