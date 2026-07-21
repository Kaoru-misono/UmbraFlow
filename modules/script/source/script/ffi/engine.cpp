#include <script/engine.hpp>

#include <core/utility/scope-exit.hpp>
#include <domain/error.hpp>

#include <cstddef>
#include <cstdlib>
#include <string>

// Luau's C headers are third-party and do not build clean under the project's
// /W4 /WX profile; a module has no CMakeLists to mark them external, so wrap the
// includes exactly as the repo's other vendored FFI does (ffi/png-decoder.cpp).
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
        auto topError(lua_State* thread) -> std::string
        {
            char const* text = lua_tostring(thread, -1);
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
        return Engine{std::make_unique<Impl>(state)};
    }

    auto Engine::runNumber(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<double>
    {
        lua_State* state = m_impl->m_state;

        auto options = lua_CompileOptions{};
        options.optimizationLevel = 1;
        options.debugLevel = 1;

        std::size_t bytecodeSize = 0;
        // SAFETY: luau_compile allocates the bytecode buffer with malloc; the caller
        // owns it. The scope guard frees it on every exit path (safe after load,
        // which copies the bytecode into the VM).
        char* bytecode = luau_compile(
            source.data(),
            source.size(),
            &options,
            &bytecodeSize
        );
        if (bytecode == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "luau_compile returned null"
            );
        }
        auto bytecodeGuard = scopeExit(
            [bytecode]() noexcept
            {
                // SAFETY: pairs the malloc inside luau_compile.
                std::free(bytecode);
            }
        );

        lua_State* thread = lua_newthread(state);
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
            auto message = "luau_load failed: " + topError(thread);
            lua_pop(state, 1);
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        int const runStatus = lua_resume(thread, nullptr, 0);
        if (runStatus != LUA_OK)
        {
            auto message = "script error: " + topError(thread);
            lua_pop(state, 1);
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        double const value = lua_gettop(thread) >= 1
            ? lua_tonumber(thread, -1)
            : 0.0;
        lua_pop(state, 1);
        return value;
    }
}
