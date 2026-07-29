#include <script/testing/cancel-probe.hpp>

#include "allocator.hpp"
#include "cancellation.hpp"
#include "environment.hpp"
#include "sandbox.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
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
        // Host C function bound as the global mark(). Its sole upvalue is a
        // lightuserdata pointing at the host-owned counter; each call bumps it.
        auto markCallback(lua_State* thread) -> int
        {
            // SAFETY: upvalue 1 is the lightuserdata installed by
            // installMarkCounter on this VM -- a pointer to the uint64 counter the
            // caller owns and keeps alive until after the VM is closed. No
            // other value is ever stored there, so casting the opaque pointer
            // back to uint64* is sound. The callback runs only on the VM's owning
            // thread, so the unsynchronized bump is race-free.
            auto* counter =
                static_cast<uint64*>(lua_tolightuserdata(thread, lua_upvalueindex(1)));
            *counter += 1;
            return 0;
        }
    }

    // Bind mark() as a C closure carrying `counter` as its lightuserdata upvalue.
    // Runs on the main state before luaL_sandbox, so the global is frozen with the
    // rest yet stays callable from the sandboxed task thread. `counter` is a
    // non-owning observation of caller-owned state that the closure mutates
    // through the Luau upvalue ABI, which admits only a raw pointer -- the same
    // shape installInterrupt uses for its control block.
    auto installMarkCounter(lua_State* state, uint64* counter) -> void
    {
        lua_pushlightuserdata(state, counter);
        lua_pushcclosure(state, &markCallback, "mark", 1);
        lua_setglobal(state, "mark");
    }

    auto probeCancellation(
        std::string_view source,
        std::string_view chunkName,
        uint64 budgetTicks
    ) -> CancellationProbe
    {
        // Host-owned counter the bound mark() bumps. It lives for the whole run
        // and is read only after the VM is closed below, so the lightuserdata
        // pointer handed into the sandbox stays valid throughout.
        uint64 markCount = 0;

        // No memory ceiling: this probe isolates cancellation, so only the
        // instruction budget (or nothing, for the positive control) may break.
        auto quota   = MemoryQuota{};
        auto control = InterruptState{.budgetTicks = budgetTicks};

        // SAFETY: createStateWithQuota installs `quota` as the allocator ledger
        // and `control` is armed as the interrupt block below; both must outlive
        // the state, and both are declared above it and outlive the close guard.
        // Mirrors the Engine's own build order (openlibs -> sandbox -> interrupt
        // -> run) so the probe exercises the real cancellation path.
        lua_State* state = createStateWithQuota(&quota);
        if (state == nullptr)
        {
            return CancellationProbe{};
        }
        auto stateGuard = scopeExit(
            [state]() noexcept
            {
                lua_close(state);
            }
        );

        luaL_openlibs(state);
        installInterrupt(state, &control);
        auto const sandboxed = installSandbox(
            state,
            EngineConfig{
                .installHostTables =
                    [&markCount](lua_State* s) -> Status
                    {
                        installMarkCounter(s, &markCount);
                        return ok();
                    },
                .projectGlobals = {std::string{"mark"}},
            },
            &control
        );
        if (!sandboxed)
        {
            return CancellationProbe{};
        }

        auto const ran =
            runNumberInProjectEnvironment(state, source, chunkName, &control, nullptr);
        bool const cancelled =
            !ran.has_value()
            && automationErrorKind(ran.error()) == AutomationErrorKind::Cancelled;

        return CancellationProbe{.cancelled = cancelled, .markCount = markCount};
    }
}
