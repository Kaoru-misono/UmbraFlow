#include "sandbox.hpp"

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <cstddef>
#include <cstdlib>
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
#include <luacode.h>
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
        [[nodiscard]]
        auto topError(lua_State* thread) -> std::string
        {
            char const* text = lua_tostring(thread, -1);
            return text != nullptr
                ? std::string{text}
                : std::string{"(non-string error value)"};
        }

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

        auto deepFreezeInto(
            lua_State* state,
            int index,
            std::unordered_set<void const*>& visited
        ) -> void
        {
            int const table = lua_absindex(state, index);
            if (!visited.insert(lua_topointer(state, table)).second)
            {
                return;
            }

            // Freeze the metatable BEFORE the table itself: lua_getmetatable
            // needs nothing writable, but leaving a metatable mutable would let
            // a script rewrite __index/__newindex and monkey-patch around the
            // frozen table it guards.
            if (lua_getmetatable(state, table) != 0)
            {
                deepFreezeInto(state, -1, visited);
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
                    deepFreezeInto(state, -1, visited);
                }
                lua_pop(state, 1);
            }
        }
    }

    auto deepFreeze(lua_State* state, int index) -> void
    {
        auto visited = std::unordered_set<void const*>{};
        deepFreezeInto(state, index, visited);
    }

    auto installSandbox(
        lua_State* state,
        HostTableInstaller const& installHostTables
    ) -> void
    {
        if (installHostTables)
        {
            installHostTables(state);
        }

        // Nil the globals luaL_sandbox does NOT remove (verified on 0.730). A
        // script that spawns its own coroutine escapes the interrupt-driven
        // cancel; debug can uninstall hooks and read outside the sandbox;
        // getfenv/setfenv/newproxy are environment escapes. The host uses
        // coroutines and debug only from C, never through these globals.
        nilGlobal(state, "getfenv");
        nilGlobal(state, "setfenv");
        nilGlobal(state, "newproxy");
        nilGlobal(state, "coroutine");
        nilGlobal(state, "debug");

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
        // point: leaving it would hand the phase-3 seeded umbra:random a second,
        // unaudited way to be reseeded from script.
        nilLibraryField(state, "math", "randomseed");

        luaL_sandbox(state);
    }

    auto runNumberOnThread(
        lua_State* mainState,
        std::string_view source,
        std::string_view chunkName,
        InterruptState const* control
    ) -> Result<double>
    {
        auto options              = lua_CompileOptions{};
        options.optimizationLevel = 1;
        options.debugLevel        = 1;

        std::size_t bytecodeSize = 0;
        // SAFETY: luau_compile allocates the bytecode buffer with malloc; the
        // caller owns it. The scope guard frees it on every exit path (safe
        // after load, which copies the bytecode into the VM). A SYNTAX error
        // does NOT return null — it is encoded as error bytecode and surfaces at
        // luau_load below; null is returned ONLY on allocation failure, hence
        // InternalInvariant.
        char* bytecode = luau_compile(
            source.data(),
            source.size(),
            &options,
            &bytecodeSize
        );
        if (bytecode == nullptr)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "luau_compile allocation failed (returned null)"
            );
        }
        auto bytecodeGuard = scopeExit(
            [bytecode]() noexcept
            {
                // SAFETY: pairs the malloc inside luau_compile; freeing a
                // caller-owned C buffer at the FFI boundary is intentional here.
                // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
                std::free(bytecode);
            }
        );

        // lua_newthread pushes the coroutine onto the main state's stack. A
        // scope guard restores the stack top on EVERY exit — including a throw
        // from the std::string allocations below — so the thread is always
        // popped and can never accumulate across repeated calls.
        int const stackBase = lua_gettop(mainState);
        lua_State* thread = lua_newthread(mainState);
        auto threadGuard = scopeExit(
            [mainState, stackBase]() noexcept
            {
                lua_settop(mainState, stackBase);
            }
        );

        // Give the task its own global table (proxying reads to the frozen main
        // globals) so writes stay isolated to this run.
        luaL_sandboxthread(thread);

        auto const name = std::string{chunkName};
        int const loadStatus = luau_load(
            thread,
            name.c_str(),
            bytecode,
            bytecodeSize,
            0
        );
        if (loadStatus != LUA_OK)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "luau_load failed: " + topError(thread)
            );
        }

        int const runStatus = lua_resume(thread, nullptr, 0);
        if (runStatus == LUA_BREAK || (control != nullptr && control->broken))
        {
            // The interrupt hard-cancelled this thread (stop token, instruction
            // budget, or deadline). The thread is abandoned — the scope guard
            // pops it and it is never resumed. A break is not a script-level
            // error and cannot be caught by pcall.
            //
            // The broken flag is checked alongside LUA_BREAK because a break
            // raised inside a non-yieldable C frame surfaces as an ordinary
            // runtime error (LUA_ERRRUN, "attempt to break across C-call
            // boundary") instead. Classifying that as a recoverable script
            // error would misreport a host control signal as a Tier B failure.
            return fail(
                AutomationErrorKind::Cancelled,
                "task hard-cancelled (lua_break); the task thread is abandoned"
            );
        }
        if (runStatus == LUA_YIELD)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "script yielded; the substrate does not resume yields"
            );
        }
        if (runStatus != LUA_OK)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "script error: " + topError(thread)
            );
        }

        return lua_gettop(thread) >= 1
            ? lua_tonumber(thread, -1)
            : 0.0;
    }
}
