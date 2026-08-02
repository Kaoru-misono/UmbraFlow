#pragma once

#include <core/types/integer.hpp>

#include <chrono>
#include <stop_token>
#include <string>

// Luau's opaque VM handle, forward-declared so this internal header never pulls
// in the Luau C headers (which compile only under the pragma-wrapped includes in
// the .cpp layer). Declared at global scope to match Luau's own typedef, so the
// name resolves to the same type once <lua.h> is visible in a .cpp.
struct lua_State;

namespace uf::script
{
    // Per-task-generation cancellation and budget state read by the VM interrupt
    // callback. It is a member of Engine::Impl, which is heap-pinned, so the
    // pointer handed to lua_callbacks->userdata stays valid and address-stable
    // for the whole life of the VM. Only `cancellation` is touched off-thread (a
    // watchdog sets the atomic behind the stop_token); every other field is read
    // and written solely by the interrupt on the VM thread, so no field but the
    // token needs synchronization.
    struct InterruptState final
    {
        // What tripped the interrupt, recorded at the instant it broke the
        // thread.
        //
        // IT IS RECORDED BECAUSE THE BREAK ITSELF CARRIES NO REASON. All three
        // triggers land on the same lua_break and the host reported all three
        // with one sentence naming none of them, which is how a session dying on
        // the clock was misdiagnosed three times over -- as console control
        // groups, as a second process, and as redirected stdio -- before anyone
        // measured the wall clock.
        //
        // None doubles as "this thread was never broken", so there is no second
        // flag that could disagree with it; see broken() below.
        enum class BreakCause : uint8
        {
            None,
            StopToken,
            InstructionBudget,
            Deadline,
        };

        // External stop source for hard cancellation. A default-constructed
        // token never requests a stop. The interrupt polls it at each safepoint.
        std::stop_token cancellation{};

        // Instruction budget expressed as a count of interrupt invocations; the
        // interrupt breaks once `ticks` reaches it. Zero disables the budget.
        uint64 budgetTicks{0};

        // Interrupt invocations counted so far in this VM generation (cumulative
        // across every runNumber call on the owning Engine).
        uint64 ticks{0};

        // Wall-clock ceiling on steady_clock for the unit of script running now;
        // the interrupt breaks once now() reaches it. time_point::max() means "no
        // deadline". The Engine re-anchors it at the start of every run, so it
        // measures the running script and never the VM's age -- see
        // EngineConfig::maxRuntime for why that distinction is the whole point.
        std::chrono::steady_clock::time_point deadline{
            std::chrono::steady_clock::time_point::max()
        };

        // When this VM was built, and when the unit of script running now
        // started. Both are read only to describe a break: a cancellation that
        // cannot say how long the thing it killed had been running is the one
        // that costs a day of wrong diagnoses.
        std::chrono::steady_clock::time_point vmStartedAt{
            std::chrono::steady_clock::now()
        };

        std::chrono::steady_clock::time_point runStartedAt{
            std::chrono::steady_clock::now()
        };

        // The instant the interrupt broke the thread. It carries no meaning
        // until `cause` is set, and `cause` is what says so; it is stamped here
        // rather than read from the clock later because the same sentence is
        // reported again on every call the spent generation refuses afterwards,
        // where a fresh now() would report a figure that grows on its own.
        std::chrono::steady_clock::time_point brokenAt{};

        // Which trigger broke the thread, written the instant the interrupt
        // issues lua_break. It never returns to None: a hard cancel spends the
        // whole VM generation, so the owning Engine turns terminal once this is
        // set and reports the same reason on every call it refuses afterwards.
        BreakCause cause{BreakCause::None};

        // Whether this VM's interrupt has broken a thread. Derived from `cause`
        // rather than stored beside it, so the fact and its reason cannot drift
        // apart.
        [[nodiscard]] auto broken() const noexcept -> bool
        {
            return cause != BreakCause::None;
        }
    };

    // Install the yield-and-abandon interrupt callback on `state`'s VM and point
    // its userdata at `control`. Must run on the main state before any task
    // thread executes; `control` must outlive the VM. Luau shares one callback
    // block across every coroutine of the VM, so this arms cancellation for all
    // task threads at once.
    auto installInterrupt(lua_State* state, InterruptState* control) -> void;

    // One sentence naming which trigger broke the thread and what it had
    // measured when it did: how long the unit of script had run against its
    // ceiling, how many ticks against the budget, and how long the VM had been
    // alive.
    //
    // The host reports it at both boundaries a cancelled generation is visible
    // from -- the failing call, and every call refused afterwards -- because the
    // second is the only one an agent sees once its session is over.
    [[nodiscard]]
    auto describeBreak(InterruptState const& control) -> std::string;
}
