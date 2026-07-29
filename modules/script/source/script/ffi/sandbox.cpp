#include "sandbox.hpp"

#include "environment.hpp"

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <optional>
#include <string>
#include <unordered_set>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; a manifest-driven module has no CMakeLists to mark them
// external, so wrap the includes exactly as the repo's other vendored FFI does
// (image/ffi/png-decoder.cpp).
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include <lua.h>
#include <lualib.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace uf::script
{
    namespace
    {
        // Remove a global name outright (script sees nil). Must run while the
        // global table is still writable, i.e. before luaL_sandbox.
        auto nilGlobal(lua_State* state, char const* name) -> void
        {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }

        // Remove `table.field` when `table` is a live library table. Must run
        // before the table is frozen by luaL_sandbox.
        auto nilLibraryField(
            lua_State* state,
            char const* table,
            char const* field
        ) -> void
        {
            lua_getglobal(state, table);
            if (lua_istable(state, -1))
            {
                lua_pushnil(state);
                lua_setfield(state, -2, field);
            }
            lua_pop(state, 1);
        }

        // Checks the metatable at `metatable` against the two rules deepFreeze
        // enforces. Both are stated at deepFreeze's declaration; the short form
        // is that a metatable without __metatable can be cloned away, and a
        // function __index is a hole the yield protocol could not survive.
        [[nodiscard]]
        auto checkMetatableShape(lua_State* state, int metatable) -> Status
        {
            lua_rawgetfield(state, metatable, "__metatable");
            bool const protectedMetatable = !lua_isnil(state, -1);
            lua_pop(state, 1);
            if (!protectedMetatable)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a frozen host object has a metatable with no __metatable "
                    "field, so table.clone could copy it into a mutable forgery "
                    "carrying the same metatable"
                );
            }

            lua_rawgetfield(state, metatable, "__index");
            bool const indexIsCallable = lua_isfunction(state, -1);
            lua_pop(state, 1);
            if (indexIsCallable)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a frozen host object has a function __index; host object "
                    "__index must be a table"
                );
            }
            return ok();
        }

        [[nodiscard]]
        auto deepFreezeInto(
            lua_State* state,
            int index,
            std::unordered_set<void const*>& visited
        ) -> Status
        {
            int const table = lua_absindex(state, index);
            if (!visited.insert(lua_topointer(state, table)).second)
            {
                return ok();
            }

            // Freeze the metatable BEFORE the table itself: lua_getmetatable
            // needs nothing writable, but leaving a metatable mutable would let
            // a script rewrite __index/__newindex and monkey-patch around the
            // frozen table it guards.
            if (lua_getmetatable(state, table) != 0)
            {
                UF_TRY(checkMetatableShape(state, lua_gettop(state)));
                UF_TRY(deepFreezeInto(state, -1, visited));
                lua_pop(state, 1);
            }

            lua_setreadonly(state, table, 1);

            lua_pushnil(state);
            while (lua_next(state, table) != 0)
            {
                // key is at -2, value at -1; recurse into nested tables, then
                // pop the value and keep the key for the next lua_next.
                if (lua_istable(state, -1))
                {
                    UF_TRY(deepFreezeInto(state, -1, visited));
                }
                lua_pop(state, 1);
            }
            return ok();
        }
    }

    auto deepFreeze(lua_State* state, int index) -> Status
    {
        // A rule violation returns from the middle of a metatable check or of a
        // lua_next walk, both of which leave values on the stack. Restoring the
        // top here keeps the failure path as balanced as the success path, so a
        // caller that reports the error does not also inherit stack garbage.
        int const stackBase = lua_gettop(state);
        auto stackGuard = scopeExit(
            [state, stackBase]() noexcept
            {
                lua_settop(state, stackBase);
            }
        );

        auto visited = std::unordered_set<void const*>{};
        return deepFreezeInto(state, index, visited);
    }

    auto deepFreezeMetatable(lua_State* state, int index) -> Status
    {
        int const stackBase = lua_gettop(state);
        auto stackGuard = scopeExit(
            [state, stackBase]() noexcept
            {
                lua_settop(state, stackBase);
            }
        );

        int const metatable = lua_absindex(state, index);
        UF_TRY(checkMetatableShape(state, metatable));

        auto visited = std::unordered_set<void const*>{};
        return deepFreezeInto(state, metatable, visited);
    }

    auto installSandbox(
        lua_State* state,
        EngineConfig const& config,
        InterruptState const* control
    ) -> Status
    {
        int const stackBase = lua_gettop(state);
        auto stackGuard = scopeExit(
            [state, stackBase]() noexcept
            {
                lua_settop(state, stackBase);
            }
        );

        // Strip the dangerous names FIRST, before any Lua code -- the framework
        // bundle included -- has run.
        //
        // The design's stated boot order loaded the framework before this step.
        // That left a capture window: the framework environment chains __index
        // to the main globals, so a module could bind `local getfenv = getfenv`
        // at load time and keep that reference for the whole generation, past
        // the nilling. It was harmless only while the framework exported nothing
        // to project code, which stopped being true the moment it started
        // exporting ctx. Nothing in the bundle legitimately wants any of these:
        // time and randomness reach the framework through the private capability
        // surface, and the environment escapes are exactly what the two-
        // environment split exists to close.
        //
        // Nil the globals luaL_sandbox does NOT remove (verified on 0.730). A
        // script that spawns its own coroutine escapes the interrupt-driven
        // cancel; debug can uninstall hooks and read outside the sandbox;
        // getfenv/setfenv/newproxy are environment escapes. getfenv is the
        // precise counterpart of `_G` once two environments exist: for a Lua
        // closure luaB_getfenv returns THAT closure's env table (lbaselib.cpp),
        // so calling it on any framework value would hand back the entire
        // framework environment. `_G` is luaopen_base's self-reference to the
        // global table -- a live alias door that reaches every global around the
        // AST resource closure, e.g. `_G.umbra:capture()` or
        // `rawget(_G, 'umbra')`; luaL_sandbox only freezes it, so it stays a
        // readable handle unless removed here. Nilling the `_G` name leaves the
        // underlying global table (LUA_GLOBALSINDEX) and every real global
        // intact; only the reflexive handle is gone. The host uses coroutines and
        // debug only from C, never through these globals.
        nilGlobal(state, "getfenv");
        nilGlobal(state, "setfenv");
        nilGlobal(state, "newproxy");
        nilGlobal(state, "coroutine");
        nilGlobal(state, "debug");
        nilGlobal(state, "gcinfo");
        nilGlobal(state, "_G");

        // Conservative determinism floor: drop the residual wall-clock and RNG
        // entry points until the host provides a logical clock and seeded RNG
        // (D9, phase 3). Loader/bytecode escapes (load*, dofile, string.dump)
        // and os process controls are already absent under Luau's base lib and
        // luaL_sandbox.
        nilLibraryField(state, "os", "time");
        nilLibraryField(state, "os", "clock");
        nilLibraryField(state, "os", "date");
        nilLibraryField(state, "math", "random");
        // randomseed is inert once random is gone, but it is still an RNG entry
        // point: leaving it would hand the seeded ctx:random a second, unaudited
        // way to be reseeded from script.
        nilLibraryField(state, "math", "randomseed");

        // The framework runs under its own environment, because the whole point
        // of the split is that trusted code never shares a global table with a
        // project script. Its private capability surface is built first and
        // handed to each module as a chunk argument, so the primitives are
        // upvalues of trusted closures and are bound to no name in either
        // environment.
        installFrameworkEnvironment(state);

        auto privateCapabilities = std::optional<int>{};
        if (config.installPrivateCapabilities)
        {
            int const before = lua_gettop(state);
            UF_TRY(config.installPrivateCapabilities(state));
            if (lua_gettop(state) != before + 1 || !lua_istable(state, -1))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "the private capability installer must leave exactly one "
                    "table on the stack"
                );
            }
            privateCapabilities = lua_gettop(state);
        }

        UF_TRY(
            loadFrameworkModules(
                state,
                config.frameworkModules,
                privateCapabilities,
                control
            )
        );

        // The surface has reached every module that will ever hold it, so drop
        // the host's own reference: from here on the only way to name it is to
        // be one of those closures.
        lua_settop(state, stackBase);

        if (config.installHostTables)
        {
            UF_TRY(config.installHostTables(state));
        }

        luaL_sandbox(state);

        // Built last, from what the steps above left standing, so a name this
        // function removed can never reappear in the project environment.
        return installProjectEnvironmentPrototype(
            state,
            config.projectGlobals,
            config.frameworkProjectGlobals
        );
    }
}
