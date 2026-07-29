#pragma once

#include "cancellation.hpp"

#include <script/engine.hpp>

#include <core/error/result.hpp>

// The public script/engine.hpp forward-declares `struct lua_State;` and defines
// HostTableInstaller, EngineConfig and deepFreeze; this internal header reuses
// them rather than redeclaring them, so the seam has a single definition.

namespace uf::script
{
    // Apply the task sandbox to a freshly opened main state, in the boot order
    // the three-layer design fixes:
    //
    //   1. build the framework environment and load `config.frameworkModules`
    //      under it, freezing each module's exports;
    //   2. run `config.installHostTables`, which registers and deep-freezes the
    //      host tables as ordinary globals;
    //   3. nil the survivors luaL_sandbox leaves behind
    //      (getfenv/setfenv/newproxy/gcinfo/coroutine/debug and `_G`, the
    //      reflexive global-table alias) and the residual clock/random globals
    //      (os.time/os.clock/os.date/math.random/math.randomseed);
    //   4. luaL_sandbox, freezing the base libraries and the global table;
    //   5. build the project environment prototype -- an explicit whitelist with
    //      no metatable, and so no __index chain to the framework environment or
    //      to the main globals -- from what step 4 left standing, including
    //      `config.projectGlobals`.
    //
    // Assumes luaL_openlibs already ran on `state`. `config.installHostTables`
    // may be empty, in which case no host tables are registered. `control` is
    // the VM's armed interrupt block, used to classify a hard cancel that lands
    // while a framework module runs; pass null when no interrupt is armed.
    //
    // Any failure -- an installer error, a framework module that does not load,
    // a whitelisted global that is absent -- is returned unchanged for the
    // caller to propagate, so a VM generation can fail for a reason its host
    // stated rather than silently coming up crippled.
    [[nodiscard]]
    auto installSandbox(
        lua_State* state,
        EngineConfig const& config,
        InterruptState const* control
    ) -> Status;
}
