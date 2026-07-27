#pragma once

#include "cancellation.hpp"

#include <script/engine.hpp>

#include <core/error/result.hpp>

#include <string_view>

// The public script/engine.hpp forward-declares `struct lua_State;` and defines
// HostTableInstaller; this internal header reuses both rather than redeclaring
// them, so the seam has a single definition.

namespace uf::script
{
    // Apply the task sandbox to a freshly opened main state, in the hardening
    // ledger's order: register and deep-freeze host tables, nil the survivors
    // luaL_sandbox leaves behind (getfenv/setfenv/newproxy/coroutine/debug), nil
    // the residual clock/random globals (os.time/os.clock/os.date/math.random),
    // then luaL_sandbox to freeze the base libraries and the global table.
    // Assumes luaL_openlibs already ran on `state`. `installHostTables` may be
    // empty, in which case no host tables are registered.
    auto installSandbox(
        lua_State* state,
        HostTableInstaller const& installHostTables
    ) -> void;

    // Recursively mark the table at stack `index`, every table reachable from
    // its values, and every metatable on the way, read-only. luaL_sandbox's
    // readonly is shallow, so nested host tables need this walk; a writable
    // metatable would reopen the same monkey-patch hole the walk closes.
    // Cycle-safe: each table is visited once.
    auto deepFreeze(lua_State* state, int index) -> void;

    // Compile, load, and run `source` on a fresh luaL_sandboxthread spun from
    // `mainState`; return its sole numeric result (the last value, or 0.0 when
    // the result is absent or non-numeric). Compile, load, and runtime errors
    // are recoverable failures. The task thread is always popped from
    // `mainState`, so repeated calls never accumulate threads.
    //
    // Pass the VM's armed `control` so a hard cancel is classified here, at the
    // single place that knows the run outcome: a break that fires in a
    // non-yieldable C frame (a table.sort comparator, a string.gsub callback)
    // surfaces as an ordinary runtime error rather than LUA_BREAK, and only
    // `control->broken` distinguishes it from a genuine script error. A null
    // `control` means no cancellation is armed on this VM.
    [[nodiscard]]
    auto runNumberOnThread(
        lua_State* mainState,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control
    ) -> Result<double>;
}
