#include <core/time/monotonic-time.hpp>

#include <doctest/doctest.h>

TEST_CASE("monotonic time addition reports overflow")
{
    using Instant = uf::MonotonicInstant;
    using Duration = Instant::Duration;
    using TimePoint = Instant::TimePoint;

    auto const start = Instant::fromTimePoint(TimePoint{Duration{10}});
    auto const end = start.checkedAdd(Duration{5});

    if (!end.has_value())
    {
        FAIL("Adding a small duration unexpectedly overflowed");
        return;
    }
    CHECK(end->saturatingDurationSince(start) == Duration{5});
    CHECK(start.saturatingDurationSince(*end) == Duration::zero());

    auto const maximum = Instant::fromTimePoint(TimePoint{Duration::max()});
    CHECK_FALSE(maximum.checkedAdd(Duration{1}).has_value());
}

TEST_CASE("monotonic duration subtraction saturates")
{
    using Instant = uf::MonotonicInstant;
    using Duration = Instant::Duration;
    using TimePoint = Instant::TimePoint;

    auto const minimum = Instant::fromTimePoint(TimePoint{Duration::min()});
    auto const maximum = Instant::fromTimePoint(TimePoint{Duration::max()});

    CHECK(maximum.saturatingDurationSince(minimum) == Duration::max());
}
