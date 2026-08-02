#pragma once

#include <core/types/integer.hpp>

#include <string_view>

// Luau's opaque VM handle, forward-declared to keep the Luau C headers out of
// this header; global scope matches Luau's own typedef.
struct lua_State;

namespace uf::script::testing
{
    // Binds a host mark() global on `state` that bumps `*counter` on each call.
    // Runs on the main state before luaL_sandbox (from an
    // EngineConfig::installHostTables installer), so the global is frozen with
    // the rest yet stays callable from the sandboxed task thread. `counter` is a
    // non-owning observation of caller-owned state that must outlive the VM. It
    // is the host-visible witness a Tier C discriminator asserts never runs
    // after a cancelled call, which proves script-level uncatchability instead
    // of inferring it from the run's error kind.
    auto installMarkCounter(lua_State* state, uint64* counter) -> void;

    // Outcome of a cancellation probe run. The Engine boundary reports Cancelled
    // the instant InterruptState::broken is set, and so would report it even if
    // a swallowed break had let the script run on; this carries a host-visible
    // record of whether the script executed the statement AFTER a runaway
    // section.
    struct CancellationProbe final
    {
        // True when the run ended in a hard break (LUA_BREAK, surfaced by the
        // shared runner as Cancelled). False when the script ran to completion.
        bool cancelled{false};

        // Times the script reached the host-bound mark() call. A clean,
        // uncatchable lua_break unwinds the whole task coroutine, so a script
        // whose mark() sits after a cancelled runaway section never advances
        // this past 0, while a cancel a pcall could swallow would drive it up.
        uint64 markCount{0};
    };

    // Build a sandboxed, interrupt-armed VM that additionally binds a host
    // function mark(), then run `source` on a task thread. A non-zero
    // `budgetTicks` is the instruction budget the interrupt trips on, so a
    // runaway loop in `source` is hard-cancelled through the exact same
    // lua_break path the Engine uses; zero disables every break lever and lets
    // the script run to completion (the positive control that proves mark() is
    // wired and reachable). Luau-free header; not part of the public Engine
    // surface.
    [[nodiscard]]
    auto probeCancellation(
        std::string_view source,
        std::string_view chunkName,
        uint64 budgetTicks
    ) -> CancellationProbe;
}
