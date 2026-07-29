#include <script/testing/capability-probe.hpp>

#include <script/engine.hpp>

#include <core/error/result.hpp>
#include <core/utility/scope-exit.hpp>

#include <domain/error.hpp>

#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>

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
#include <luacode.h>
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
        [[nodiscard]]
        auto topError(lua_State* state) -> std::string
        {
            char const* text = lua_tostring(state, -1);
            return text != nullptr
                ? std::string{text}
                : std::string{"(non-string error value)"};
        }
    }

    auto scriptedPrivateCapabilities(
        PrivateCapabilityInstaller inner,
        std::string source,
        std::string chunkName
    ) -> PrivateCapabilityInstaller
    {
        return [inner     = std::move(inner),
                source    = std::move(source),
                chunkName = std::move(chunkName)](lua_State* state) -> Status
        {
            int const base = lua_gettop(state);
            auto stackGuard = scopeExit(
                [state, base]() noexcept
                {
                    lua_settop(state, base);
                }
            );

            if (inner)
            {
                UF_TRY(inner(state));
                if (lua_gettop(state) != base + 1 || !lua_istable(state, -1))
                {
                    return fail(
                        AutomationErrorKind::InternalInvariant,
                        "the wrapped capability installer must leave exactly one "
                        "table on the stack"
                    );
                }
            }

            // The chunk runs under the main globals, which the boot has already
            // stripped of the clock and RNG entry points by the time a private
            // capability installer is called. It is neither environment a task
            // sees: a fake surface is host code that happens to be written in
            // Luau, so it is loaded the way the host loads its own boot steps.
            auto options              = lua_CompileOptions{};
            options.optimizationLevel = 1;
            options.debugLevel        = 1;

            std::size_t bytecodeSize = 0;
            // SAFETY: luau_compile allocates the bytecode buffer with malloc and
            // hands ownership to the caller; the guard below frees it on every
            // exit path, which is safe after load because load copies the
            // bytecode into the VM. A syntax error is encoded as error bytecode
            // and surfaces at luau_load, so null means allocation failure alone.
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
                    // caller-owned C buffer at the FFI boundary is intentional.
                    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
                    std::free(bytecode);
                }
            );

            if (
                luau_load(state, chunkName.c_str(), bytecode, bytecodeSize, 0)
                != LUA_OK
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the scripted capability surface failed to load: "
                        + topError(state)
                );
            }

            int argumentCount = 0;
            if (inner)
            {
                lua_pushvalue(state, base + 1);
                argumentCount = 1;
            }
            if (lua_pcall(state, argumentCount, 1, 0) != LUA_OK)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "the scripted capability surface raised while building: "
                        + topError(state)
                );
            }
            if (!lua_istable(state, -1))
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "a scripted capability surface must return one table"
                );
            }

            // Drop the wrapped surface from under the result, so the installer
            // grows the stack by exactly the one table its contract promises.
            if (inner)
            {
                lua_replace(state, base + 1);
            }
            stackGuard.release();
            return ok();
        };
    }
}
