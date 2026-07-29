#include <task/deterministic.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <array>
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
    }
}
