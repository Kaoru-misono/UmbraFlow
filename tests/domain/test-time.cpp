#include <domain/time.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <chrono>

namespace uf
{
    namespace
    {
        auto instantAt(
            MonotonicInstant::Duration duration
        ) -> MonotonicInstant
        {
            return MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{duration}
            );
        }

        template <typename Duration>
        auto clockDuration(Duration duration) -> MonotonicInstant::Duration
        {
            return std::chrono::duration_cast<MonotonicInstant::Duration>(duration);
        }
    }

    TEST_CASE("monotonic ordering and saturating duration agree")
    {
        auto const first = instantAt(MonotonicInstant::Duration{1});
        auto const fiveMilliseconds = clockDuration(std::chrono::milliseconds{5});
        auto const second = checkedAddMonotonic(first, fiveMilliseconds);

        REQUIRE(second.has_value());
        CHECK(first < *second);
        CHECK(second->saturatingDurationSince(first) == fiveMilliseconds);
        CHECK(first.saturatingDurationSince(*second) == MonotonicInstant::Duration::zero());
    }

    TEST_CASE("elapsed nanoseconds saturate at zero")
    {
        using Instant = MonotonicInstant;

        auto const first = Instant::fromTimePoint(Instant::TimePoint{});
        auto const threeMicroseconds = clockDuration(std::chrono::microseconds{3});
        auto const second = checkedAddMonotonic(first, threeMicroseconds);

        REQUIRE(second.has_value());
        CHECK(elapsedNanosecondsSince(*second, first) == uint64{3'000});
        CHECK(elapsedNanosecondsSince(first, *second) == uint64{0});
    }

    TEST_CASE("monotonic deadline overflow is rejected")
    {
        using Duration = MonotonicInstant::Duration;

        auto const first = instantAt(Duration{1});
        auto const unchanged = checkedAddMonotonic(first, Duration::zero());
        REQUIRE(unchanged.has_value());
        CHECK(*unchanged == first);

        auto const lastValid = checkedAddMonotonic(
            first,
            Duration::max() - Duration{1}
        );
        REQUIRE(lastValid.has_value());
        CHECK(lastValid->timePoint().time_since_epoch() == Duration::max());

        auto const overflow = checkedAddMonotonic(first, Duration::max());

        REQUIRE_FALSE(overflow.has_value());
        auto const kind = automationErrorKind(overflow.error());
        REQUIRE(kind.has_value());
        CHECK(kind == AutomationErrorKind::InternalInvariant);
    }

    TEST_CASE("negative clock durations fail closed")
    {
        auto const result = checkedAddMonotonic(
            instantAt(MonotonicInstant::Duration{1}),
            MonotonicInstant::Duration{-1}
        );

        REQUIRE_FALSE(result.has_value());
        auto const kind = automationErrorKind(result.error());
        REQUIRE(kind.has_value());
        CHECK(kind == AutomationErrorKind::InternalInvariant);
    }
}
