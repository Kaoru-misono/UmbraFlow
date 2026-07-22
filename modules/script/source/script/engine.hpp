#pragma once

#include <core/error/result.hpp>

#include <memory>
#include <string_view>

namespace uf::script
{
    // Owns one embedded Luau VM (lua_State), closed on destruction. RAII; Luau
    // types are confined to the implementation and never appear in this header.
    // NOT thread-safe: all calls must occur on the owning thread (the step-3
    // cancellation watchdog may only set an atomic flag, never touch the VM).
    class Engine final
    {
        class Impl;
        std::unique_ptr<Impl> m_impl;

        explicit Engine(std::unique_ptr<Impl> p_impl) noexcept;

    public:
        Engine(Engine&&) noexcept;
        auto operator=(Engine&&) noexcept -> Engine&;
        ~Engine();

        // Create a VM with the standard libraries opened. Fails if the VM cannot
        // be allocated.
        [[nodiscard]] static auto create() -> Result<Engine>;

        // Compile, load, and run `source` on a fresh coroutine; return the script's
        // sole numeric result (the LAST value if it returns several; 0.0 if it returns
        // nothing numeric). A compile, load, or runtime error is a recoverable failure.
        // Step-2 minimal: the script is NOT sandboxed and NOT cancellable — an infinite
        // loop hangs the caller until step-3 (see engine.cpp create()).
        [[nodiscard]]
        auto runNumber(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<double>;
    };
}
