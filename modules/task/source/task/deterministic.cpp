#include <task/deterministic.hpp>

#include <core/error/contracts.hpp>
#include <core/types/integer.hpp>

namespace uf::task
{
    namespace
    {
        // The PCG32 constants (O'Neill's reference generator). The multiplier is
        // the LCG step; the stream selector fixes one output sequence so a seed
        // fully determines the numbers. Both are load-bearing for reproducibility:
        // changing either changes every task's random sequence, so they are pinned
        // exactly like the Luau compiler tag.
        constexpr auto k_pcgMultiplier     = uint64{6364136223846793005};
        constexpr auto k_pcgStreamSelector = uint64{1442695040888963407};
    }

    DeterministicRng::DeterministicRng(uint64 seed) noexcept
        : m_state{0}
        , m_inc{(k_pcgStreamSelector << 1U) | uint64{1}}
    {
        // The reference seeding routine: step once, fold in the seed, step again,
        // so the seed perturbs the whole state rather than only its low bits.
        (void)nextUint32();
        m_state += seed;
        (void)nextUint32();
    }

    auto DeterministicRng::nextUint32() noexcept -> uint32
    {
        // Unsigned wraparound is the LCG's defined arithmetic, not overflow: uint64
        // multiplication and addition are modulo 2^64 by the standard, which is
        // exactly the recurrence PCG specifies. No signed value takes part.
        uint64 const oldState = m_state;
        m_state               = oldState * k_pcgMultiplier + m_inc;

        uint32 const xorShifted =
            static_cast<uint32>(((oldState >> 18U) ^ oldState) >> 27U);
        uint32 const rotation = static_cast<uint32>(oldState >> 59U);
        return (xorShifted >> rotation) | (xorShifted << ((0U - rotation) & 31U));
    }

    auto DeterministicRng::nextUnitDouble() noexcept -> double
    {
        // 53 random bits over 2^53 gives a uniform double in [0, 1). The shift and
        // the multiply by 2^-53 are exact, so the result carries no rounding that
        // could differ between runs.
        uint64 const high = uint64{nextUint32()} << 32U;
        uint64 const low  = uint64{nextUint32()};
        uint64 const bits = high | low;
        return static_cast<double>(bits >> 11U) * 0x1.0p-53;
    }

    auto DeterministicRng::boundedUint64(uint64 span) noexcept -> uint64
    {
        UF_ASSERT(span != 0);

        // Reject the lowest (2^64 mod span) outputs so the accepted range is an
        // exact multiple of span, removing the modulo bias a bare `% span` leaves.
        // (0 - span) is unsigned negation = 2^64 - span, whose remainder mod span
        // is 2^64 mod span.
        uint64 const threshold = (uint64{0} - span) % span;
        for (;;)
        {
            uint64 const high  = uint64{nextUint32()} << 32U;
            uint64 const low   = uint64{nextUint32()};
            uint64 const value = high | low;
            if (value >= threshold)
            {
                return value % span;
            }
        }
    }
}
