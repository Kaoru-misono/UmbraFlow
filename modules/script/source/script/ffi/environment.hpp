#pragma once

#include "cancellation.hpp"

#include <script/engine.hpp>

#include <core/error/result.hpp>

#include <optional>
#include <span>
#include <string>
#include <string_view>

// The public script/engine.hpp forward-declares `struct lua_State;` and defines
// FrameworkModule; this internal header reuses both rather than redeclaring
// them, so the seam has a single definition.

namespace uf::script
{
    // Builds the framework environment and registers it in the VM registry,
    // leaving the stack as it found it.
    //
    // Environment isolation in Luau is per-closure, not per-thread: luau_load
    // takes the env table a chunk's closure carries (lvmload.cpp), and a new
    // thread's globals table is copied from its parent (lstate.cpp), so the
    // luaL_sandboxthread proxy shape cannot separate two trust levels on one VM.
    // Two explicit env tables can.
    //
    // The framework environment is a writable table whose frozen metatable
    // chains __index to the main globals, so trusted framework code sees the
    // admitted standard library and keeps its own globals off the main table.
    // That proxy shape is deliberate HERE and forbidden for the project
    // environment, which is exactly the `_G` escape the design rules out.
    auto installFrameworkEnvironment(lua_State* state) -> void;

    // Pushes the registered framework environment onto `state`'s stack.
    auto pushFrameworkEnvironment(lua_State* state) -> void;

    // Loads and runs each module in order under the framework environment,
    // deep-freezes the value it returns, and binds that value in the framework
    // environment under the module's name, so a later module can reach an
    // earlier one and nothing outside the framework can reach either.
    //
    // `privateCapabilities`, when present, is a `state` stack index holding the
    // private capability surface; every module receives it as its single chunk
    // argument (`local native = ...`). That is what makes the primitives closure
    // upvalues of trusted code rather than keys of a reachable table: nothing
    // binds the surface into either environment, so the only way to hold it is
    // to have been handed it here.
    //
    // A module that fails to compile, load, run, or freeze fails the whole
    // generation: the framework is first-party and compiled into the binary, so
    // a broken module is a broken host rather than bad user input.
    [[nodiscard]]
    auto loadFrameworkModules(
        lua_State* state,
        std::span<FrameworkModule const> modules,
        std::optional<int> privateCapabilities,
        InterruptState const* control
    ) -> Status;

    // Builds the project environment prototype and registers it, frozen, in the
    // VM registry. Must run after luaL_sandbox, because the prototype copies the
    // surviving globals by value and a name removed later would still be in it.
    //
    // The prototype is an explicit whitelist: the deterministic base functions
    // and libraries this file names one by one, plus `hostGlobals`, the names
    // the host installer registered, plus `frameworkGlobals`, the framework
    // module names whose frozen exports the project may name. It carries NO
    // metatable, so there is no __index chain to the framework environment or to
    // the main globals -- the one structural property that makes the whole
    // denial list hold, and the reason publishing a framework export copies the
    // value rather than opening a route to its neighbours. A whitelisted name
    // that is absent from its source table fails InternalInvariant rather than
    // silently producing a thinner environment.
    [[nodiscard]]
    auto installProjectEnvironmentPrototype(
        lua_State* state,
        std::span<std::string const> hostGlobals,
        std::span<std::string const> frameworkGlobals
    ) -> Status;

    // Pushes a fresh, writable project environment -- a shallow copy of the
    // registered prototype -- onto `state`'s stack. One per run, so globals a
    // run writes die with it, and the frozen values it shares stay frozen.
    [[nodiscard]]
    auto pushProjectEnvironment(lua_State* state) -> Status;

    // Compiles, loads and runs `source` on a fresh thread spun from `mainState`,
    // with the table at `environmentIndex` (a `mainState` stack index) as the
    // chunk's environment, and returns its sole numeric result (the last value,
    // or 0.0 when the result is absent or non-numeric). Compile, load, and
    // runtime errors are recoverable failures. The task thread is always popped
    // from `mainState`, so repeated calls never accumulate threads.
    //
    // Pass the VM's armed `control` so a hard cancel is classified here, at the
    // single place that knows the run outcome: a break that fires in a
    // non-yieldable C frame (a table.sort comparator, a string.gsub callback)
    // surfaces as an ordinary runtime error rather than LUA_BREAK, and only
    // `control->broken` distinguishes it from a genuine script error. A null
    // `control` means no cancellation is armed on this VM.
    //
    // `classify` observes the config's classifier for the raised value of a run
    // that failed; null, or an empty classifier, reports every raise as
    // InvalidResource. It is borrowed for the call only -- the EngineConfig-owned
    // std::function it names outlives every call the Engine makes.
    [[nodiscard]]
    auto runNumberInEnvironment(
        lua_State* mainState,
        int environmentIndex,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control,
        RaisedErrorClassifier const* classify
    ) -> Result<double>;

    // Runs `source` under a project environment freshly built for this call, and
    // discards that environment afterwards. This is the one entry point a
    // project chunk ever takes: every run of a task script goes through it, so
    // "the project script sees only the project environment" is a property of
    // the runner rather than of each caller remembering to build one.
    [[nodiscard]]
    auto runNumberInProjectEnvironment(
        lua_State* mainState,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control,
        RaisedErrorClassifier const* classify
    ) -> Result<double>;
}
