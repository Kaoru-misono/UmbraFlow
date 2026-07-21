#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <cmath>
#include <limits>

TEST_CASE("checked integer arithmetic rejects overflow")
{
    using uf::checkedAdd;
    using uf::checkedDivide;
    using uf::checkedMultiply;
    using uf::checkedRemainder;
    using uf::checkedSubtract;

    auto constexpr unsignedMaximum = std::numeric_limits<uf::uint32>::max();
    CHECK(
        checkedAdd<uf::uint32>(uf::uint32{40}, uf::uint32{2})
        == uf::uint32{42}
    );
    CHECK_FALSE(checkedAdd<uf::uint32>(unsignedMaximum, uf::uint32{1}).has_value());
    CHECK_FALSE(checkedSubtract<uf::uint32>(uf::uint32{0}, uf::uint32{1}).has_value());

    auto constexpr signedMinimum = std::numeric_limits<uf::int64>::min();
    auto constexpr signedMaximum = std::numeric_limits<uf::int64>::max();
    CHECK_FALSE(checkedAdd<uf::int64>(signedMaximum, uf::int64{1}).has_value());
    CHECK_FALSE(checkedSubtract<uf::int64>(signedMinimum, uf::int64{1}).has_value());
    CHECK(
        checkedMultiply<uf::int64>(uf::int64{-3}, uf::int64{7})
        == uf::int64{-21}
    );
    CHECK(checkedMultiply<uf::int64>(signedMinimum, uf::int64{1}) == signedMinimum);
    CHECK_FALSE(checkedMultiply<uf::int64>(signedMinimum, uf::int64{-1}).has_value());
    CHECK_FALSE(checkedMultiply<uf::int64>(signedMaximum, uf::int64{2}).has_value());
    CHECK_FALSE(checkedDivide<uf::int64>(signedMinimum, uf::int64{-1}).has_value());
    CHECK_FALSE(checkedDivide<uf::int64>(uf::int64{1}, uf::int64{0}).has_value());
    CHECK(checkedDivide<uf::int64>(uf::int64{42}, uf::int64{7}) == uf::int64{6});
    CHECK_FALSE(checkedRemainder<uf::int64>(signedMinimum, uf::int64{-1}).has_value());
    CHECK(checkedRemainder<uf::int64>(uf::int64{43}, uf::int64{7}) == uf::int64{1});
}

TEST_CASE("checked casts reject narrowing and non-finite input")
{
    using uf::checkedCast;
    using uf::checkedIntegralCast;

    CHECK(checkedCast<uf::uint16>(uf::uint32{42}) == uf::uint16{42});
    CHECK_FALSE(checkedCast<uf::uint16>(uf::uint32{70'000}).has_value());
    CHECK_FALSE(checkedCast<uf::uint32>(uf::int32{-1}).has_value());

    CHECK(checkedIntegralCast<uf::int32>(42.0) == uf::int32{42});
    CHECK_FALSE(checkedIntegralCast<uf::int32>(42.5).has_value());
    CHECK_FALSE(checkedIntegralCast<uf::uint8>(256.0).has_value());
    CHECK_FALSE(checkedIntegralCast<uf::uint64>(std::ldexp(1.0, 64)).has_value());
    CHECK_FALSE(
        checkedIntegralCast<uf::int32>(std::numeric_limits<double>::infinity()).has_value()
    );
    CHECK_FALSE(
        checkedIntegralCast<uf::int32>(std::numeric_limits<double>::quiet_NaN()).has_value()
    );
}
