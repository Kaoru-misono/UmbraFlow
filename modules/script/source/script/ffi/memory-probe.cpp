#include <script/testing/memory-probe.hpp>

#include "allocator.hpp"
#include "environment.hpp"
#include "sandbox.hpp"

#include <core/utility/scope-exit.hpp>

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
    auto measureMemory(
        std::string_view source,
        std::string_view chunkName,
        std::size_t limitBytes
    ) -> MemoryUsage
    {
        auto quota = MemoryQuota{.limitBytes = limitBytes};

        // SAFETY: createStateWithQuota installs `quota` as the allocator ledger;
        // it must outlive the state, and it does -- `quota` is declared above the
        // state and read again only after the close guard below has run. Returns
        // null on host allocation failure.
        lua_State* state = createStateWithQuota(&quota);
        if (state == nullptr)
        {
            return MemoryUsage{};
        }

        {
            auto stateGuard = scopeExit(
                [state]() noexcept
                {
                    lua_close(state);
                }
            );

            luaL_openlibs(state);

            // Neither the boot nor the run is under test here: a failure (an
            // over-quota breach, say) still leaves the ledger to be checked once
            // the VM is closed, so both results are consumed and discarded
            // deliberately. No interrupt is armed on this quota-only probe,
            // hence the null control block.
            if (installSandbox(state, EngineConfig{}, nullptr))
            {
                auto const ran = runNumberInProjectEnvironment(
                    state,
                    source,
                    chunkName,
                    nullptr
                );
                (void)ran;
            }
        }
        // The close guard has run: every byte the VM held was returned through
        // the allocator, so `used` is the post-close residual and `peak` is the
        // high-water mark observed over the VM's life.

        return MemoryUsage{.peak = quota.peak, .residual = quota.used};
    }
}
