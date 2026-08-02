#pragma once

#include <core/types/integer.hpp>

namespace uf::task
{
    // A script reads no clock of any kind. Waiting is evidence-based (ctx:wait
    // against a host-minted deadline) or declarative (ctx:settle), and both are
    // host effects the trace records; enforcing a real wall-clock budget stays the
    // host interrupt's job.

    // A task's sole source of randomness (ctx:random), replacing the math.random
    // the sandbox removed. A self-contained PCG32 rather than a std::mt19937
    // paired with a std::uniform_int_distribution, whose library implementations
    // are not guaranteed identical across toolchains and would break same-machine
    // reproducibility. The seed is host-injected (TaskContextConfig::randomSeed).
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
        // rejection sampling. `span` must be non-zero; the caller (ctx:random's
        // binding) guarantees a non-empty interval before calling.
        [[nodiscard]]
        auto boundedUint64(uint64 span) noexcept -> uint64;
    };
}
