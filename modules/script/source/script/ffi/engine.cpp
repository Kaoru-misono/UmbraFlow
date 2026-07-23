#include <script/engine.hpp>

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <cstddef>
#include <cstdlib>
#include <string>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; a module has no CMakeLists to mark them external, so wrap the
// includes exactly as the repo's other vendored FFI does (image/ffi/png-decoder.cpp).
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
    class Engine::Impl final
    {
    public:
        lua_State* m_state;

        explicit Impl(lua_State* p_state) noexcept
            : m_state{p_state}
        {
        }

        Impl(Impl const&) = delete;
        Impl(Impl&&) = delete;
        auto operator=(Impl const&) -> Impl& = delete;
        auto operator=(Impl&&) -> Impl& = delete;

        ~Impl()
        {
            if (m_state != nullptr)
            {
                // SAFETY: m_state is the owning lua_State handle from luaL_newstate.
                lua_close(m_state);
            }
        }
    };

    namespace
    {
        [[nodiscard]]
        auto topError(lua_State* p_thread) -> std::string
        {
            char const* text = lua_tostring(p_thread, -1);
            return text != nullptr
                ? std::string{text}
                : std::string{"(non-string error value)"};
        }
    }

    Engine::Engine(std::unique_ptr<Impl> p_impl) noexcept
        : m_impl{std::move(p_impl)}
    {
    }

    Engine::Engine(Engine&&) noexcept = default;
    auto Engine::operator=(Engine&&) noexcept -> Engine& = default;
    Engine::~Engine() = default;

    auto Engine::create() -> Result<Engine>
    {
        // SAFETY: luaL_newstate allocates the VM and returns null on failure.
        lua_State* state = luaL_newstate();
        if (state == nullptr)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                "luaL_newstate returned null"
            );
        }
        luaL_openlibs(state);
        // TODO(cpp-debt): step-2 minimal Engine — NOT sandboxed and NOT cancellable
        // yet. Scripts run with the full stdlib on a shared global table (globals leak
        // across runNumber calls) and an infinite loop hangs the caller. Step 3 installs
        // luaL_sandbox + interrupt/lua_break cancellation + the dangerous-globals nil-list
        // (getfenv/setfenv/newproxy/coroutine/debug), per
        // docs/plans/2026-07-21-luau-integration-plan.md.
        return Engine{std::make_unique<Impl>(state)};
    }

    auto Engine::runNumber(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<double>
    {
        lua_State* state = m_impl->m_state;

        auto options              = lua_CompileOptions{};
        options.optimizationLevel = 1;
        options.debugLevel        = 1;

        std::size_t bytecodeSize = 0;
        // SAFETY: luau_compile allocates the bytecode buffer with malloc; the caller
        // owns it. The scope guard frees it on every exit path (safe after load,
        // which copies the bytecode into the VM). A SYNTAX error does NOT return null —
        // it is encoded as error bytecode and surfaces at luau_load below; null is
        // returned ONLY on allocation failure, hence InternalInvariant (as with
        // luaL_newstate returning null in create()).
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

        // lua_newthread pushes the coroutine onto the main state's stack. A scope guard
        // restores the stack top on EVERY exit — including a throw from the std::string
        // allocations below — so the thread is always popped and can never accumulate
        // across repeated runNumber calls.
        int const stackBase = lua_gettop(state);
        lua_State* thread = lua_newthread(state);
        auto threadGuard = scopeExit(
            [state, stackBase]() noexcept
            {
                lua_settop(state, stackBase);
            }
        );

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
        if (runStatus == LUA_YIELD)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "script yielded; the step-2 Engine does not resume yields"
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
