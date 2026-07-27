#pragma once

#include <core/types/integer.hpp>

#include <chrono>
#include <stop_token>

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
        // External stop source for hard cancellation. A default-constructed
        // token never requests a stop. The interrupt polls it at each safepoint.
        std::stop_token cancellation{};

        // Instruction budget expressed as a count of interrupt invocations; the
        // interrupt breaks once `ticks` reaches it. Zero disables the budget.
        uint64 budgetTicks{0};

        // Interrupt invocations counted so far in this VM generation (cumulative
        // across every runNumber call on the owning Engine).
        uint64 ticks{0};

        // Wall-clock ceiling on steady_clock; the interrupt breaks once now()
        // reaches it. time_point::max() means "no deadline".
        std::chrono::steady_clock::time_point deadline{
            std::chrono::steady_clock::time_point::max()
        };

        // Set the instant the interrupt issues lua_break. It never clears: a hard
        // cancel spends the whole VM generation, so the owning Engine turns
        // terminal once this is observed.
        bool broken{false};
    };

    // Install the yield-and-abandon interrupt callback on `state`'s VM and point
    // its userdata at `control`. Must run on the main state before any task
    // thread executes; `control` must outlive the VM. Luau shares one callback
    // block across every coroutine of the VM, so this arms cancellation for all
    // task threads at once.
    auto installInterrupt(lua_State* state, InterruptState* control) -> void;
}
