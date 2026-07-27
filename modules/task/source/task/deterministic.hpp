#pragma once

#include <core/types/integer.hpp>

namespace uf::task
{
    // A task's logical clock, exposed to scripts as umbra:now(). It is fully
    // virtualized: it holds NO wall clock and reads NO steady_clock. Each read
    // returns a whole-millisecond logical count that starts at zero and advances by
    // one fixed logical tick, so the sequence a task observes is a pure function of
    // how many times it read the clock -- and therefore byte-identical on every run
    // of the same script over the same inputs. That reproducibility is load-bearing
    // for veto #4 ("identical on one machine, 1000x").
    //
    // This is deliberately NOT real elapsed time, and the class is named for the
    // property it now actually has. An earlier version returned the wall time
    // elapsed since construction; that value varies run to run with execution speed
    // and silently made any now()-branching script non-reproducible even at the
    // same seed, since the replay contract records only the RNG seed and never a
    // now() reading. Virtualizing the clock is what closes that hole: a script may
    // treat now() as a monotone, reproducible ordinal, never as a measurement of
    // real duration. Enforcing a real wall-clock budget (max runtime) is the host
    // interrupt's job, never the script's.
    //
    // This logical clock is unrelated to the engine's observation lease, an
    // engine-internal wall-clock fuse that expires a stale observation before an
    // action consumes it; the lease is never visible to a script and never drives
    // now().
    class DeterministicClock final
    {
        // Whole logical milliseconds already handed out. int64 to match
        // umbra:now()'s return; a task would have to read the clock 2^63 times to
        // approach the ceiling, far past any instruction budget, and readMillis
        // saturates rather than wraps if it ever did.
        int64 m_logicalMillis{0};

    public:
        DeterministicClock() noexcept = default;

        // The current logical time in whole milliseconds, after which the clock
        // advances by one fixed logical tick. Successive reads therefore strictly
        // increase (umbra:now() is non-decreasing) and are identical across runs
        // regardless of how fast the machine executed. Reading advances the clock,
        // so this is not a const observer -- the mutation is deliberately visible in
        // the signature rather than hidden behind a mutable member.
        [[nodiscard]]
        auto readMillis() noexcept -> int64;
    };

    // A task's sole source of randomness (umbra:random), replacing the math.random
    // the sandbox removed. It is a self-contained PCG32 generator -- a fixed,
    // version-stable algorithm implemented here rather than a std::mt19937 paired
    // with a std::uniform_int_distribution, whose library implementations are not
    // guaranteed identical across toolchains and would break same-machine
    // reproducibility. The seed is host-injected (TaskContextConfig::randomSeed);
    // the same seed drawn in the same order yields the same sequence, on this
    // machine, every run.
    class DeterministicRng final
    {
        uint64 m_state;
        uint64 m_inc;

    public:
        // Seeds the generator. Every seed selects one fixed output stream, so two
        // generators built from the same seed are byte-for-byte identical.
        explicit DeterministicRng(uint64 seed) noexcept;

        // The next 32-bit output word of the stream.
        [[nodiscard]]
        auto nextUint32() noexcept -> uint32;

        // The next double in [0, 1). Assembled from exact integer operations and a
        // multiply by an exact power of two, so the result is reproducible with no
        // rounding ambiguity.
        [[nodiscard]]
        auto nextUnitDouble() noexcept -> double;

        // A uniformly distributed value in [0, span) with no modulo bias, using
        // rejection sampling. `span` must be non-zero; the caller (umbra:random's
        // binding) guarantees a non-empty interval before calling.
        [[nodiscard]]
        auto boundedUint64(uint64 span) noexcept -> uint64;
    };
}
