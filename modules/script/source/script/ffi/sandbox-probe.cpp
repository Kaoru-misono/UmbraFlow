#include <script/testing/sandbox-probe.hpp>

#include "environment.hpp"
#include "sandbox.hpp"

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <string>

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
        // `{ __index = { inherited = 5 }, __metatable = "probe.host" }`,
        // deep-freeze the whole shape (recursively read-only, metatable
        // included), then bind it as a global. Registered before luaL_sandbox, so
        // the global binding itself is frozen with the rest.
        //
        // The metatable carries every property deepFreeze now demands of a
        // project-visible host object: a table __index (so a script inheriting
        // through it reads a frozen table rather than running host code) and a
        // __metatable field (so getmetatable hands back a label, and table.clone
        // refuses the object outright). It is therefore both the fixture for the
        // walk's metatable arm and the fixture proving the rules are enforced --
        // dropping either field makes deepFreeze reject this table.
        [[nodiscard]]
        auto installSyntheticHostTable(lua_State* state) -> Status
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
            lua_pushstring(state, "probe.host");
            lua_setfield(state, -2, "__metatable");
            lua_setmetatable(state, -2);

            UF_TRY(deepFreeze(state, -1));
            lua_setglobal(state, "host");
            return ok();
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
        UF_TRY(
            installSandbox(
                state,
                EngineConfig{
                    .installHostTables = &installSyntheticHostTable,
                    .projectGlobals    = {std::string{"host"}},
                },
                nullptr
            )
        );

        // No interrupt is armed on this sandbox-only probe, hence the null
        // control block.
        return runNumberInProjectEnvironment(state, source, chunkName, nullptr);
    }
}
