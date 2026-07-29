#pragma once

#include "cancellation.hpp"

#include <script/engine.hpp>

#include <core/error/result.hpp>

// The public script/engine.hpp forward-declares `struct lua_State;` and defines
// HostTableInstaller, EngineConfig and deepFreeze; this internal header reuses
// them rather than redeclaring them, so the seam has a single definition.

namespace uf::script
{
    // Apply the task sandbox to a freshly opened main state, in this boot order:
    //
    //   1. nil the survivors luaL_sandbox leaves behind
    //      (getfenv/setfenv/newproxy/gcinfo/coroutine/debug and `_G`, the
    //      reflexive global-table alias) and the residual clock/random globals
    //      (os.time/os.clock/os.date/math.random/math.randomseed);
    //   2. build the framework environment;
    //   3. run `config.installPrivateCapabilities`, which leaves the private
    //      capability surface on the stack;
    //   4. load `config.frameworkModules` under the framework environment with
    //      that surface as each module's chunk argument, freezing each module's
    //      exports, then drop the host's reference to the surface;
    //   5. run `config.installHostTables`, which registers and deep-freezes the
    //      host data tables as ordinary globals;
    //   6. luaL_sandbox, freezing the base libraries and the global table;
    //   7. build the project environment prototype -- an explicit whitelist with
    //      no metatable, and so no __index chain to the framework environment or
    //      to the main globals -- from what step 6 left standing, including
    //      `config.projectGlobals` and `config.frameworkProjectGlobals`.
    //
    // Step 1 precedes every step that runs Lua, which is what stops a framework
    // module from binding a dangerous global at load time and holding it past
    // the nilling. The design's own §7 ordering ran the framework first; that
    // window is closed here deliberately.
    //
    // Assumes luaL_openlibs already ran on `state`. Either installer may be
    // empty, in which case nothing is registered for it. `control` is the VM's
    // armed interrupt block, used to classify a hard cancel that lands while a
    // framework module runs; pass null when no interrupt is armed.
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
