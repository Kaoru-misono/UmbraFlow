#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <engine/session.hpp>

#include <optional>

namespace uf::task
{
    // Draws the next process-wide stamp for one family of host-minted handles.
    // Every store that hands a script a name for something the host keeps takes
    // one at construction, so a handle left over from a spent generation is
    // rejected instead of colliding with a live ordinal in the next one. One
    // counter serves all of them: the stamp only has to be unique.
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

    // The host-side ledger of open observation cycles: it holds AT MOST ONE, so
    // "the resolution proof and action came from the same frame" stays a comparison the host
    // performs rather than a discipline the Luau framework keeps -- with one
    // entry, mixing two frames is not rejected, it cannot be expressed.
    //
    // NOT thread-safe: every method runs on the VM's owning thread. Destroying
    // the ledger releases whatever cycle is still open, which is the host's
    // backstop when a generation is torn down mid-script.
    class CycleLedger final
    {
        // The one open cycle: the frame it retains, and how much of the cycle's
        // own read budget has been spent on it. It holds no semantic resolution --
        // the trusted Runtime mints authorization against this cycle's ticket. What
        // the ledger owns is the part a Luau table could not be trusted with: one
        // frame, spent once.
        struct OpenCycle final
        {
            engine::Observation observation;
            uint64              ordinal{};

            // How many text reads have already been charged to this cycle. It
            // lives on the cycle rather than on the run because that is the scope
            // the budget is stated in: a wait loop that reads once per poll is
            // normal, and a bound spanning the whole run would either stop that
            // loop or be so large it stops nothing.
            uint32 reads{};

            // How many crops have been charged to this cycle, on the same
            // reasoning and as its own dimension: a whole-panel PNG encode, a line
            // read and a SAD comparison are not comparable units.
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
        // one VM generation, so a fresh stamp per instance is what makes a ticket
        // from a spent generation rejectable instead of accidentally valid in the
        // next one.
        CycleLedger() noexcept;

        [[nodiscard]] auto isOpen() const noexcept -> bool;

        // The one-cycle rule, stated once. Opening a second cycle is a framework
        // bug rather than a script error, so it is InternalInvariant and not a
        // Tier B failure a script can catch and retry.
        [[nodiscard]] auto requireClosed() const -> Status;

        // Fails StaleObservation unless `ticket` names the open cycle. Under the
        // one-cycle rule this is the entire staleness check: a ticket that is not
        // the open cycle's names a cycle that no longer exists.
        [[nodiscard]] auto requireOpen(CycleTicket ticket) const -> Status;

        // The same check for the ordinal carried by Host evidence.
        [[nodiscard]] auto requireOpenEvidence(uint64 ordinal) const -> Status;

        // Opens the cycle over `observation` and returns the ticket that names it.
        // Precondition: requireClosed() has just succeeded -- a separate call so
        // the caller can refuse before spending a whole capture on a frame this
        // ledger could not hold.
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

        // How many text reads the open cycle has already spent, and the charge for
        // more; same precondition as observation(), and the charge takes a count
        // because a block read costs one recognition per line it located.
        [[nodiscard]] auto readsCharged() const noexcept -> uint32;

        auto chargeReads(uint32 count) noexcept -> void;

        // The same pair for the crop budget. Same precondition, same reason the
        // count belongs to the ledger rather than to a caller.
        [[nodiscard]] auto cropsCharged() const noexcept -> uint32;

        auto chargeCrop() noexcept -> void;

        // Releases whatever cycle is open, reporting whether there was one: NOT
        // reachable from any script and must not become so, because its one caller
        // is the exploration session sweeping between two agent-supplied chunks,
        // which is the host cleaning up after a bracket it owns.
        auto closeOpen() noexcept -> bool;

        // Spends the cycle `ticket` names: the frame leaves the ledger and the
        // ticket dies here, before the input that follows can do anything with the
        // frame. Fails StaleObservation for a ticket that names no open cycle. One
        // entry point serves the click and the keystroke alike, since the page
        // requirement that once distinguished them moved to layer two.
        [[nodiscard]] auto spend(CycleTicket ticket) -> Result<engine::Observation>;
    };
}
