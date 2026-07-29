#include <task/deterministic.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <vector>

namespace uf::task
{
    namespace
    {
        // Draws `count` raw 32-bit words from a generator seeded with `seed`.
        [[nodiscard]]
        auto drawWords(uint64 seed, std::size_t count) -> std::vector<uint32>
        {
            auto rng   = DeterministicRng{seed};
            auto words = std::vector<uint32>{};
            words.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                words.emplace_back(rng.nextUint32());
            }
            return words;
        }

        TEST_CASE("The same seed replays an identical sequence and a different seed diverges")
        {
            // Same seed, same draw order -> byte-for-byte identical, the property a
            // trace-recorded seed relies on to replay a run. At least a hundred
            // numbers, so a generator that only agreed on its first few outputs
            // would be caught.
            auto const first  = drawWords(0x1234'5678'9ABC'DEF0, 128);
            auto const second = drawWords(0x1234'5678'9ABC'DEF0, 128);
            CHECK(first == second);

            // A different seed selects a different sequence.
            auto const other = drawWords(0x0FED'CBA9'8765'4321, 128);
            CHECK(other != first);
        }

        TEST_CASE("nextUnitDouble stays within [0, 1)")
        {
            auto rng = DeterministicRng{0xABCDEF};
            for (int sample = 0; sample < 1000; ++sample)
            {
                double const value = rng.nextUnitDouble();
                CHECK(value >= 0.0);
                CHECK(value < 1.0);
            }
        }

        TEST_CASE("boundedUint64 stays in range and reaches both endpoints")
        {
            // Span 6: every draw must land in [0, 6), and across enough samples both
            // the low endpoint 0 and the high endpoint 5 must actually appear, so a
            // mapping that quietly dropped an endpoint would fail.
            constexpr uint64 span = 6;
            auto             rng  = DeterministicRng{42};

            bool sawLow  = false;
            bool sawHigh = false;
            for (int sample = 0; sample < 20'000; ++sample)
            {
                uint64 const value = rng.boundedUint64(span);
                REQUIRE(value < span);
                sawLow  = sawLow || value == 0;
                sawHigh = sawHigh || value == span - 1;
            }
            CHECK(sawLow);
            CHECK(sawHigh);
        }

        TEST_CASE("boundedUint64 is roughly uniform across its buckets")
        {
            // A coarse but falsifiable unbiasedness check: over a large sample every
            // bucket count should sit near N/span. The tolerance is wide (a biased
            // modulo mapping skews far past it) yet nowhere near the noise floor --
            // a bucket's standard deviation here is about a hundred counts, so a
            // fifteen-percent band is a dozen-plus sigma and never flakes.
            constexpr std::size_t bucketCount = 8;
            constexpr int         samples     = 100'000;
            auto                  rng         = DeterministicRng{7};

            auto counts = std::array<int, bucketCount>{};
            for (int sample = 0; sample < samples; ++sample)
            {
                uint64 const value = rng.boundedUint64(bucketCount);
                REQUIRE(value < bucketCount);
                ++counts[static_cast<std::size_t>(value)];
            }

            constexpr double expected = static_cast<double>(samples) / bucketCount;
            for (int const count : counts)
            {
                CHECK(static_cast<double>(count) > expected * 0.85);
                CHECK(static_cast<double>(count) < expected * 1.15);
            }
        }

        TEST_CASE("The logical clock ignores wall time and reproduces across runs")
        {
            // Regression for the veto #4 hole where ctx:now() returned wall time
            // elapsed since construction. That reading varied run to run with
            // execution speed, so a now()-branching script diverged even at the same
            // seed. The virtualized clock advances by a fixed logical tick per read
            // and consults no wall clock, so the real time a run happens to take can
            // never enter a reading.
            //
            // Two clocks are built at the same instant. Only `a` observes real wall
            // time pass between its reads. A wall-derived clock would fold that gap
            // into `a`'s second reading and disagree with `b`; the virtualized clock
            // must not.
            auto a = DeterministicClock{};
            auto b = DeterministicClock{};

            int64 const a0 = a.readMillis();

            // Force well over a millisecond of real wall time to elapse, seen only
            // between `a`'s two reads. A busy wait on steady_clock guarantees a lower
            // bound on the elapsed time and, because each iteration calls now(),
            // cannot be optimized away.
            constexpr auto k_gapMillis = int64{25};
            auto const     spinStart   = std::chrono::steady_clock::now();
            while (
                std::chrono::steady_clock::now() - spinStart
                < std::chrono::milliseconds{k_gapMillis}
            )
            {
            }

            int64 const a1 = a.readMillis();
            int64 const b0 = b.readMillis();
            int64 const b1 = b.readMillis();

            // The contract ctx:now() still promises: non-negative and monotone.
            CHECK(a0 >= 0);
            CHECK(a1 >= a0);

            // The ~25 ms of real time did NOT enter the reading: two reads differ by
            // a single logical tick, not by ~25. The old wall-clock implementation
            // fails this line (its delta would be about k_gapMillis).
            CHECK(a1 - a0 < k_gapMillis);

            // Reproducibility across runs: `b`, which saw none of that wall time,
            // yields the exact same first two readings as `a`. A wall-derived clock
            // would have b0 already near k_gapMillis (its construction predates the
            // spin), diverging from a0.
            CHECK(a0 == b0);
            CHECK(a1 == b1);
        }
    }
}
