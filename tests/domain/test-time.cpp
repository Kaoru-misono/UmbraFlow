#include <domain/time.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstdint>

namespace
{
    auto instantAt(
        uf::MonotonicInstant::Duration duration
    ) -> uf::MonotonicInstant
    {
        return uf::MonotonicInstant::fromTimePoint(
            uf::MonotonicInstant::TimePoint{duration}
        );
    }

    template <typename Duration>
    auto clockDuration(Duration duration) -> uf::MonotonicInstant::Duration
    {
        return std::chrono::duration_cast<uf::MonotonicInstant::Duration>(duration);
    }
}

TEST_CASE("monotonic ordering and saturating duration agree")
{
    auto const first = instantAt(uf::MonotonicInstant::Duration{1});
    auto const fiveMilliseconds = clockDuration(std::chrono::milliseconds{5});
    auto const second = uf::checkedAddMonotonic(first, fiveMilliseconds);

    REQUIRE(second.has_value());
    CHECK(first < *second);
    CHECK(second->saturatingDurationSince(first) == fiveMilliseconds);
    CHECK(first.saturatingDurationSince(*second) == uf::MonotonicInstant::Duration::zero());
}

TEST_CASE("elapsed nanoseconds saturate at zero")
{
    using Instant = uf::MonotonicInstant;

    auto const first = Instant::fromTimePoint(Instant::TimePoint{});
    auto const threeMicroseconds = clockDuration(std::chrono::microseconds{3});
    auto const second = uf::checkedAddMonotonic(first, threeMicroseconds);

    REQUIRE(second.has_value());
    CHECK(uf::elapsedNanosecondsSince(*second, first) == std::uint64_t{3'000});
    CHECK(uf::elapsedNanosecondsSince(first, *second) == std::uint64_t{0});
}

TEST_CASE("monotonic deadline overflow is rejected")
{
    using Duration = uf::MonotonicInstant::Duration;

    auto const first = instantAt(Duration{1});
    auto const unchanged = uf::checkedAddMonotonic(first, Duration::zero());
    REQUIRE(unchanged.has_value());
    CHECK(*unchanged == first);

    auto const lastValid = uf::checkedAddMonotonic(
        first,
        Duration::max() - Duration{1}
    );
    REQUIRE(lastValid.has_value());
    CHECK(lastValid->timePoint().time_since_epoch() == Duration::max());

    auto const overflow = uf::checkedAddMonotonic(first, Duration::max());

    REQUIRE_FALSE(overflow.has_value());
    auto const kind = uf::automationErrorKind(overflow.error());
    REQUIRE(kind.has_value());
    CHECK(*kind == uf::AutomationErrorKind::InternalInvariant);
}

TEST_CASE("negative clock durations fail closed")
{
    auto const result = uf::checkedAddMonotonic(
        instantAt(uf::MonotonicInstant::Duration{1}),
        uf::MonotonicInstant::Duration{-1}
    );

    REQUIRE_FALSE(result.has_value());
    auto const kind = uf::automationErrorKind(result.error());
    REQUIRE(kind.has_value());
    CHECK(*kind == uf::AutomationErrorKind::InternalInvariant);
}
