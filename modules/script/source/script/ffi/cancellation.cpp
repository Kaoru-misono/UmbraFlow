#include "cancellation.hpp"

#include <chrono>

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
        // VM safepoint callback. Luau shares one lua_Callbacks across every
        // coroutine of the VM, so this fires on whichever task thread is running
        // under lua_resume, and lua_callbacks(state)->userdata is the shared
        // InterruptState installed by installInterrupt. Cancellation is
        // yield-and-abandon: on a hit it calls lua_break (NEVER luaL_error),
        // which sets the running thread's status to LUA_BREAK; lua_resume then
        // returns LUA_BREAK and the host abandons the thread. A break is not a
        // Lua error, so a pcall in the script cannot catch it.
        auto onInterrupt(lua_State* state, int gc) -> void
        {
            // The GC-context guard MUST be the first statement (hardening-ledger
            // hard red line): the interrupt also fires during collection (gc >=
            // 0), where breaking is unsafe. Only the negative-gc safepoints (loop
            // back edges, call/ret) may break.
            if (gc >= 0)
            {
                return;
            }

            lua_Callbacks* callbacks = lua_callbacks(state);
            // SAFETY: userdata is the InterruptState installed by
            // installInterrupt on this VM. Luau documents it as never
            // overwritten, and Impl keeps it alive for the VM's whole life, so
            // this reinterpretation of the opaque pointer is sound. It is null
            // only when no control was installed (never for an Engine VM).
            auto* control = static_cast<InterruptState*>(callbacks->userdata);
            if (control == nullptr)
            {
                return;
            }

            control->ticks += 1;

            bool const cancelled = control->cancellation.stop_requested();
            bool const overBudget =
                control->budgetTicks != 0 && control->ticks >= control->budgetTicks;
            bool const overTime =
                std::chrono::steady_clock::now() >= control->deadline;

            if (cancelled || overBudget || overTime)
            {
                control->broken = true;
                // Yield-and-abandon: unwinds to lua_resume with LUA_BREAK; the
                // host drops this thread and never resumes it.
                lua_break(state);
            }
        }
    }

    auto installInterrupt(lua_State* state, InterruptState* control) -> void
    {
        lua_Callbacks* callbacks = lua_callbacks(state);
        callbacks->userdata      = control;
        callbacks->interrupt     = &onInterrupt;
    }
}
