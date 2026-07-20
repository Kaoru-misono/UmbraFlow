#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <limits>

TEST_CASE("checked integer arithmetic rejects overflow")
{
    using umbra_flow::checkedAdd;
    using umbra_flow::checkedDivide;
    using umbra_flow::checkedMultiply;
    using umbra_flow::checkedRemainder;
    using umbra_flow::checkedSubtract;

    auto constexpr unsignedMaximum = std::numeric_limits<std::uint32_t>::max();
    CHECK(
        checkedAdd<std::uint32_t>(std::uint32_t{40}, std::uint32_t{2})
        == std::uint32_t{42}
    );
    CHECK_FALSE(checkedAdd<std::uint32_t>(unsignedMaximum, std::uint32_t{1}).has_value());
    CHECK_FALSE(checkedSubtract<std::uint32_t>(std::uint32_t{0}, std::uint32_t{1}).has_value());

    auto constexpr signedMinimum = std::numeric_limits<std::int64_t>::min();
    auto constexpr signedMaximum = std::numeric_limits<std::int64_t>::max();
    CHECK_FALSE(checkedAdd<std::int64_t>(signedMaximum, std::int64_t{1}).has_value());
    CHECK_FALSE(checkedSubtract<std::int64_t>(signedMinimum, std::int64_t{1}).has_value());
    CHECK(
        checkedMultiply<std::int64_t>(std::int64_t{-3}, std::int64_t{7})
        == std::int64_t{-21}
    );
    CHECK(checkedMultiply<std::int64_t>(signedMinimum, std::int64_t{1}) == signedMinimum);
    CHECK_FALSE(checkedMultiply<std::int64_t>(signedMinimum, std::int64_t{-1}).has_value());
    CHECK_FALSE(checkedMultiply<std::int64_t>(signedMaximum, std::int64_t{2}).has_value());
    CHECK_FALSE(checkedDivide<std::int64_t>(signedMinimum, std::int64_t{-1}).has_value());
    CHECK_FALSE(checkedDivide<std::int64_t>(std::int64_t{1}, std::int64_t{0}).has_value());
    CHECK(checkedDivide<std::int64_t>(std::int64_t{42}, std::int64_t{7}) == std::int64_t{6});
    CHECK_FALSE(checkedRemainder<std::int64_t>(signedMinimum, std::int64_t{-1}).has_value());
    CHECK(checkedRemainder<std::int64_t>(std::int64_t{43}, std::int64_t{7}) == std::int64_t{1});
}

TEST_CASE("checked casts reject narrowing and non-finite input")
{
    using umbra_flow::checkedCast;
    using umbra_flow::checkedIntegralCast;

    CHECK(checkedCast<std::uint16_t>(std::uint32_t{42}) == std::uint16_t{42});
    CHECK_FALSE(checkedCast<std::uint16_t>(std::uint32_t{70'000}).has_value());
    CHECK_FALSE(checkedCast<std::uint32_t>(std::int32_t{-1}).has_value());

    CHECK(checkedIntegralCast<std::int32_t>(42.0) == std::int32_t{42});
    CHECK_FALSE(checkedIntegralCast<std::int32_t>(42.5).has_value());
    CHECK_FALSE(checkedIntegralCast<std::uint8_t>(256.0).has_value());
    CHECK_FALSE(checkedIntegralCast<std::uint64_t>(std::ldexp(1.0, 64)).has_value());
    CHECK_FALSE(
        checkedIntegralCast<std::int32_t>(std::numeric_limits<double>::infinity()).has_value()
    );
    CHECK_FALSE(
        checkedIntegralCast<std::int32_t>(std::numeric_limits<double>::quiet_NaN()).has_value()
    );
}
