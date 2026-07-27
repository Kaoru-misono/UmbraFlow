#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <chrono>
#include <memory>
#include <stop_token>
#include <string_view>

namespace uf::script
{
    // Tunables for one task VM generation. Every field is live: the cancellation
    // source and the instruction/time budgets drive the interrupt callback, and
    // the memory ceiling drives the accounting allocator that backs the VM. The
    // defaults are conservative placeholders to be calibrated against the first
    // real task. Luau types never appear here.
    struct EngineConfig final
    {
        // External stop source for hard cancellation. A default-constructed
        // token never requests a stop. The interrupt callback (later wave) reads
        // it; a watchdog thread may only set the atomic behind it and must never
        // touch the VM.
        std::stop_token cancellation{};

        // Per-task hard memory ceiling the accounting allocator enforces by
        // refusing any over-quota growth (Luau surfaces the refusal as a
        // catchable out-of-memory error, so the host is never dragged down).
        // Zero disables the ceiling. Conservative placeholder to be calibrated
        // against the first real task.
        uint64 memoryQuotaBytes{uint64{64} * 1024 * 1024};

        // Instruction budget counted by the interrupt callback (later wave).
        // Conservative placeholder pending calibration.
        uint64 interruptBudgetTicks{uint64{100'000'000}};

        // Wall-clock ceiling measured on steady_clock by the interrupt callback
        // (later wave). Placeholder default.
        std::chrono::steady_clock::duration maxRuntime{std::chrono::minutes{30}};
    };

    // Owns one embedded Luau VM (lua_State) for a single task generation: create
    // one per task, run it, then destroy it. A lua_State is never reused across
    // tasks, so mutable globals cannot leak between tasks. RAII; Luau types are
    // confined to the implementation and never appear in this header. NOT
    // thread-safe: every call runs on the owning thread. An external watchdog
    // may only set the atomic behind EngineConfig::cancellation, never the VM.
    class Engine final
    {
        class Impl;
        std::unique_ptr<Impl> m_impl;

        explicit Engine(std::unique_ptr<Impl> p_impl) noexcept;

    public:
        Engine(Engine&&) noexcept;
        auto operator=(Engine&&) noexcept -> Engine&;
        ~Engine();

        // Create a task VM with the standard libraries opened and sandboxed per
        // the hardening ledger: the dangerous survivors luaL_sandbox leaves
        // (getfenv/setfenv/newproxy/coroutine/debug) are removed, the residual
        // clock/random globals are removed, and the base libraries are frozen.
        // Fails if the VM cannot be allocated.
        [[nodiscard]]
        static auto create(EngineConfig const& config = {}) -> Result<Engine>;

        // Compile, load, and run `source` on a fresh sandboxed task thread;
        // return the script's sole numeric result (the LAST value if it returns
        // several; 0.0 if it returns nothing numeric). Each call runs on its own
        // luaL_sandboxthread, so mutable globals never leak between calls. A
        // compile, load, or runtime error is a recoverable failure. Retained as
        // the substrate's test entry point.
        [[nodiscard]]
        auto runNumber(
            std::string_view source,
            std::string_view chunkName
        ) -> Result<double>;
    };
}
