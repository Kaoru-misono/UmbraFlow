#pragma once

#include <core/types/integer.hpp>

#include <string_view>

namespace uf::script::testing
{
    // Outcome of a cancellation probe run. Unlike the Engine boundary -- which
    // reports Cancelled the instant InterruptState::broken is set, and so would
    // report Cancelled even if a swallowed break had let the script run on --
    // this carries a direct, host-visible record of whether the script executed
    // the statement AFTER a runaway section. That makes script-level
    // uncatchability observable rather than inferred.
    struct CancellationProbe final
    {
        // True when the run ended in a hard break (LUA_BREAK, surfaced by the
        // shared runner as Cancelled). False when the script ran to completion.
        bool cancelled{false};

        // Times the script reached the host-bound mark() call. A clean,
        // uncatchable lua_break unwinds the whole task coroutine, so a script
        // whose mark() sits after a cancelled runaway section never advances
        // this past 0; a cancel that a pcall could swallow would let control
        // reach mark() and drive it up. On an uncancelled run it counts the
        // calls the script actually made.
        uint64 markCount{0};
    };

    // Build a sandboxed, interrupt-armed VM that additionally binds a host
    // function mark() (each call bumps a host-owned counter), then run `source`
    // on a task thread. A non-zero `budgetTicks` is the instruction budget the
    // interrupt trips on, so a runaway loop in `source` is hard-cancelled
    // through the exact same lua_break path the Engine uses; zero disables every
    // break lever and lets the script run to completion (the positive control
    // that proves mark() is wired and reachable). Reports whether the run was
    // cancelled and how many times the script reached mark(). Luau-free header;
    // not part of the public Engine surface.
    [[nodiscard]]
    auto probeCancellation(
        std::string_view source,
        std::string_view chunkName,
        uint64 budgetTicks
    ) -> CancellationProbe;
}
