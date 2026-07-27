#include <script/testing/sandbox-probe.hpp>

#include "sandbox.hpp"

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; wrap the includes exactly as the repo's other vendored FFI
// does (image/ffi/png-decoder.cpp).
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

namespace uf::script::testing
{
    namespace
    {
        // Build `host = { flat = 7, nested = { value = 1 } }` behind a metatable
        // `{ __index = { inherited = 5 } }`, deep-freeze the whole shape
        // (recursively read-only, metatable included), then bind it as a global.
        // Registered before luaL_sandbox, so the global binding itself is frozen
        // with the rest. The metatable is what makes the walk's metatable arm
        // testable: without it, a script could reach `getmetatable(host)` and
        // rewrite `__index` to shadow the frozen table underneath.
        auto installSyntheticHostTable(lua_State* state) -> void
        {
            lua_newtable(state);
            lua_pushnumber(state, 7.0);
            lua_setfield(state, -2, "flat");

            lua_newtable(state);
            lua_pushnumber(state, 1.0);
            lua_setfield(state, -2, "value");
            lua_setfield(state, -2, "nested");

            lua_newtable(state); // metatable
            lua_newtable(state); // __index target
            lua_pushnumber(state, 5.0);
            lua_setfield(state, -2, "inherited");
            lua_setfield(state, -2, "__index");
            lua_setmetatable(state, -2);

            deepFreeze(state, -1);
            lua_setglobal(state, "host");
        }
    }

    auto runWithFrozenHostTable(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<double>
    {
        // SAFETY: luaL_newstate allocates the VM and returns null on failure;
        // the scope guard closes it on every exit path. Confined to this test
        // seam, which owns the whole VM lifetime locally.
        lua_State* state = luaL_newstate();
        if (state == nullptr)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "luaL_newstate returned null"
            );
        }
        auto stateGuard = scopeExit(
            [state]() noexcept
            {
                lua_close(state);
            }
        );

        luaL_openlibs(state);
        installSandbox(state, installSyntheticHostTable);

        // No interrupt is armed on this sandbox-only probe, hence the null
        // control block.
        return runNumberOnThread(state, source, chunkName, nullptr);
    }
}
