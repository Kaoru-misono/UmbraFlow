#pragma once

#include <core/error/result.hpp>

#include <memory>
#include <string_view>

namespace uf::script
{
    // Owns one embedded Luau VM (lua_State), closed on destruction. RAII; Luau
    // types are confined to the implementation and never appear in this header.
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

        // Compile, load, and run `source` on a fresh coroutine; return the
        // script's first numeric result (0.0 if it returns nothing numeric). A
        // compile, load, or runtime error is a recoverable failure.
        [[nodiscard]]
        auto runNumber(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<double>;
    };
}
