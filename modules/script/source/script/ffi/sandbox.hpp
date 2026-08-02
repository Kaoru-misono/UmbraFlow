#pragma once

#include "cancellation.hpp"

#include <script/engine.hpp>

#include <core/error/result.hpp>

// `struct lua_State;`, HostTableInstaller, EngineConfig and deepFreeze are
// reused from the public script/engine.hpp rather than redeclared, so the seam
// has one definition.

namespace uf::script
{
    // Apply the task sandbox to a freshly opened main state, in this boot order:
    //
    //   1. nil the survivors luaL_sandbox leaves (getfenv/setfenv/newproxy/
    //      gcinfo/coroutine/debug and `_G`, the reflexive global-table alias)
    //      and the residual clock and random globals
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
    //   7. build the project environment prototype from what step 6 left
    //      standing -- an explicit whitelist with no metatable, and so no
    //      __index chain to the framework environment or to the main globals --
    //      including `config.projectGlobals` and
    //      `config.frameworkProjectGlobals`.
    //
    // Step 1 precedes every step that runs Lua, which is what stops a framework
    // module from binding a dangerous global at load time and holding it past
    // the nilling. Assumes luaL_openlibs already ran on `state`. Either
    // installer may be empty. `control` is the VM's armed interrupt block, used
    // to classify a hard cancel that lands while a framework module runs; pass
    // null when none is armed. Any failure -- an installer error, a framework
    // module that does not load, an absent whitelisted global -- is returned
    // unchanged, so a generation fails for a reason its host stated rather than
    // silently coming up crippled.
    [[nodiscard]]
    auto installSandbox(
        lua_State* state,
        EngineConfig const& config,
        InterruptState const* control
    ) -> Status;
}
