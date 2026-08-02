#include "cancellation.hpp"

#include <chrono>
#include <format>
#include <string>

// Luau's C headers do not build clean under the project's /W4 /WX profile, and
// a manifest-driven module has no CMakeLists to mark them external; wrap them as
// image/ffi/png-decoder.cpp does.
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
        // Which trigger a break is reported under when more than one is true at
        // the same safepoint: the host's own stop request outranks a spent
        // budget, and a spent budget outranks the clock. The break is identical
        // whichever fired -- this decides only the sentence the host prints.
        [[nodiscard]]
        auto reportedCause(
            bool cancelled,
            bool overBudget,
            bool overTime
        ) noexcept -> InterruptState::BreakCause
        {
            if (cancelled)
            {
                return InterruptState::BreakCause::StopToken;
            }
            if (overBudget)
            {
                return InterruptState::BreakCause::InstructionBudget;
            }
            if (overTime)
            {
                return InterruptState::BreakCause::Deadline;
            }
            return InterruptState::BreakCause::None;
        }

        [[nodiscard]]
        auto elapsedSeconds(std::chrono::steady_clock::duration span) -> double
        {
            return std::chrono::duration<double>{span}.count();
        }

        // VM safepoint callback. Luau shares one lua_Callbacks across every
        // coroutine of the VM, so this fires on whichever task thread is running
        // under lua_resume. Cancellation is yield-and-abandon: on a hit it calls
        // lua_break (NEVER luaL_error), lua_resume returns LUA_BREAK and the host
        // abandons the thread. A break is not a Lua error, so a pcall in the
        // script cannot catch it.
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
            auto const now      = std::chrono::steady_clock::now();
            bool const overTime = now >= control->deadline;

            auto const cause = reportedCause(cancelled, overBudget, overTime);
            if (cause == InterruptState::BreakCause::None)
            {
                return;
            }

            // Stamped before the break, which does not return here: lua_break
            // unwinds to lua_resume, where the host reads this state.
            control->cause    = cause;
            control->brokenAt = now;

            lua_break(state);
        }
    }

    auto installInterrupt(lua_State* state, InterruptState* control) -> void
    {
        lua_Callbacks* callbacks = lua_callbacks(state);
        callbacks->userdata      = control;
        callbacks->interrupt     = &onInterrupt;
    }

    auto describeBreak(InterruptState const& control) -> std::string
    {
        auto const ranFor = elapsedSeconds(control.brokenAt - control.runStartedAt);
        auto const vmAge  = elapsedSeconds(control.brokenAt - control.vmStartedAt);

        switch (control.cause)
        {
        case InterruptState::BreakCause::StopToken:
            return std::format(
                "the host requested cancellation {:.1f}s into this unit of "
                "script, {:.1f}s into the VM's life",
                ranFor,
                vmAge
            );
        case InterruptState::BreakCause::InstructionBudget:
            return std::format(
                "the instruction budget is spent: {} of {} interrupt ticks, "
                "counted across the VM's whole {:.1f}s life rather than this "
                "unit of script alone",
                control.ticks,
                control.budgetTicks,
                vmAge
            );
        case InterruptState::BreakCause::Deadline:
            return std::format(
                "the wall-clock ceiling expired: this unit of script ran "
                "{:.1f}s of its {:.1f}s ceiling, {:.1f}s into the VM's life",
                ranFor,
                elapsedSeconds(control.deadline - control.runStartedAt),
                vmAge
            );
        case InterruptState::BreakCause::None:
            break;
        }
        return "no interrupt trigger was recorded";
    }
}
