#pragma once

#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <stop_token>
#include <string>

// Luau's opaque VM handle, forward-declared to keep the Luau C headers out of
// this header; global scope matches Luau's own typedef.
struct lua_State;

namespace uf::script
{
    // The farthest instant the monotonic clock can name. As a deadline it is
    // never reached, so it is both the disabled ceiling and where a ceiling
    // longer than the clock's range saturates.
    inline constexpr auto k_maximumInstant =
        MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint::max());

    // Cancellation and budget state the VM interrupt reads. Lives in the
    // heap-pinned Engine::Impl because lua_callbacks->userdata holds its address
    // for the life of the VM. Only `cancellation` is touched off-thread, so no
    // other field needs synchronization.
    struct InterruptState final
    {
        // What tripped the interrupt. All three triggers land on the same
        // lua_break, so a break that does not record its cause cannot name one
        // afterwards. None doubles as "never broken".
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

        // Ceiling for the unit of script running now; k_maximumInstant disables
        // it. The Engine re-anchors it per run, so it never measures the VM's
        // age -- see EngineConfig::maxRuntime.
        MonotonicInstant deadline{k_maximumInstant};

        // Read only to describe a break.
        MonotonicInstant vmStartedAt{MonotonicInstant::now()};

        MonotonicInstant runStartedAt{MonotonicInstant::now()};

        // Stamped at the break rather than read from the clock later, because a
        // spent generation reports the same sentence on every call it refuses
        // and a fresh now() would grow between them. `cause` says whether it has
        // been stamped, so an unbroken state needs no sentinel here.
        MonotonicInstant brokenAt{MonotonicInstant::now()};

        // Never returns to None: a break spends the whole VM generation.
        BreakCause cause{BreakCause::None};

        [[nodiscard]] auto broken() const noexcept -> bool
        {
            return cause != BreakCause::None;
        }

        // Anchor the unit of script about to run and arm its ceiling from
        // `runtimeCeiling`. Every deadline in this module is set here, so the
        // overflow policy is stated once: see the definition.
        auto beginUnitOfScript(MonotonicInstant::Duration runtimeCeiling) noexcept -> void;
    };

    // Install the yield-and-abandon interrupt callback on `state`'s VM and point
    // its userdata at `control`. Must run on the main state before any task
    // thread executes; `control` must outlive the VM. Luau shares one callback
    // block across every coroutine of the VM, so this arms cancellation for all
    // task threads at once.
    auto installInterrupt(lua_State* state, InterruptState* control) -> void;

    // One sentence naming which trigger broke the thread and what it had
    // measured: the unit of script's run time against its ceiling, ticks against
    // the budget, and the VM's age. The host reports it both at the failing call
    // and at every call refused afterwards, the latter being the only one an
    // agent sees once its session is over.
    [[nodiscard]]
    auto describeBreak(InterruptState const& control) -> std::string;
}
