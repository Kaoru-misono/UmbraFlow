#include "test-helpers.hpp"

#include <pacing.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <utility>
#include <vector>

TEST_CASE("m0 click delay rejects zero and inverted bounds")
{
    auto const malformed = std::array{
        std::pair{uf::uint64{0}, uf::uint64{0}},
        std::pair{uf::uint64{0}, uf::uint64{100}},
        std::pair{uf::uint64{100}, uf::uint64{0}},
        std::pair{uf::uint64{200}, uf::uint64{100}},
    };
    for (auto const [minimum, maximum] : malformed)
    {
        auto const result = uf::m0_demo::ClickDelay::create(minimum, maximum);
        REQUIRE_FALSE(result.has_value());
        test_m0_demo::requireErrorKind(
            result.error(),
            uf::AutomationErrorKind::InvalidResource
        );
    }

    auto const range = uf::m0_demo::ClickDelay::create(600, 1800);
    REQUIRE(range.has_value());
    CHECK(range->minimumMilliseconds() == 600U);
    CHECK(range->maximumMilliseconds() == 1800U);

    auto const fixed = uf::m0_demo::ClickDelay::create(1000, 1000);
    REQUIRE(fixed.has_value());
    CHECK(fixed->minimumMilliseconds() == 1000U);
    CHECK(fixed->maximumMilliseconds() == 1000U);
}

TEST_CASE("m0 click delay picks within its inclusive range")
{
    auto const delay = uf::m0_demo::ClickDelay::create(600, 1800);
    REQUIRE(delay.has_value());
    auto random = uf::m0_demo::SplitMix64{uf::m0_demo::g_defaultPacingSeed};
    for (auto index = 0; index < 10'000; ++index)
    {
        auto const milliseconds = delay->pickMilliseconds(random.next());
        CHECK(milliseconds >= 600U);
        CHECK(milliseconds <= 1800U);
    }

    CHECK(delay->pickMilliseconds(0) == 600U);
    CHECK(delay->pickMilliseconds(1200) == 1800U);
    CHECK(delay->pickMilliseconds(1201) == 600U);
    CHECK(
        delay->pickMilliseconds(std::numeric_limits<uf::uint64>::max())
        == 600U + (std::numeric_limits<uf::uint64>::max() % 1201U)
    );
}

TEST_CASE("m0 fixed click delay always returns the same milliseconds")
{
    auto const delay = uf::m0_demo::ClickDelay::create(1000, 1000);
    REQUIRE(delay.has_value());
    auto const values = std::array{
        uf::uint64{0},
        uf::uint64{1},
        uf::uint64{12'345},
        std::numeric_limits<uf::uint64>::max(),
    };
    for (auto const raw : values)
    {
        CHECK(delay->pickMilliseconds(raw) == 1000U);
    }
}

TEST_CASE("m0 SplitMix64 is deterministic for a seed")
{
    auto const draw = [](uf::uint64 seed)
    {
        auto random = uf::m0_demo::SplitMix64{seed};
        auto values = std::vector<uf::uint64>{};
        values.reserve(8);
        for (auto index = 0; index < 8; ++index)
        {
            values.emplace_back(random.next());
        }
        return values;
    };

    CHECK(draw(42) == draw(42));
    CHECK(draw(42) != draw(43));
    auto const sequence = draw(uf::m0_demo::g_defaultPacingSeed);
    auto advances = false;
    for (auto index = std::size_t{1}; index < sequence.size(); ++index)
    {
        if (sequence[index - 1U] != sequence[index])
        {
            advances = true;
            break;
        }
    }
    CHECK(advances);

    auto zero = uf::m0_demo::SplitMix64{0};
    CHECK(zero.next() == 0xE220'A839'7B1D'CDAFULL);
}
