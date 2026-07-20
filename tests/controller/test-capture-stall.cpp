#include <controller/detail/capture-stall.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <chrono>

namespace
{
    [[nodiscard]]
    auto automationKind(uf::Error const& error) -> uf::AutomationErrorKind
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        return *kind;
    }

    [[nodiscard]]
    auto instant(
        uf::MonotonicInstant::Duration::rep ticks
    ) -> uf::MonotonicInstant
    {
        return uf::MonotonicInstant::fromTimePoint(
            uf::MonotonicInstant::TimePoint{uf::MonotonicInstant::Duration{ticks}}
        );
    }

    [[nodiscard]]
    auto plus(
        uf::MonotonicInstant base,
        uf::MonotonicInstant::Duration duration
    ) -> uf::MonotonicInstant
    {
        auto const added = base.checkedAdd(duration);
        REQUIRE(added.has_value());
        return *added;
    }
}

TEST_CASE("stall timeout accepts the exact boundary and rejects the next tick")
{
    auto const base = instant(10'000);
    auto const timeout = uf::MonotonicInstant::Duration{1'000};
    auto const tracker = uf::controller_detail::StallTracker{
        timeout,
        base
    };

    CHECK(tracker.check(plus(base, timeout)).has_value());
    auto const result = tracker.check(
        plus(base, timeout + uf::MonotonicInstant::Duration{1})
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::CaptureStalled);
}

TEST_CASE("frame stranded in the slot is stale by arrival rather than consumption")
{
    auto const timeout = uf::MonotonicInstant::Duration{1'000};
    auto tracker = uf::controller_detail::StallTracker{
        timeout,
        instant(0)
    };
    tracker.onFrameArrived(instant(100));

    CHECK(tracker.check(instant(1'100)).has_value());
    auto const result = tracker.check(instant(1'101));
    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::CaptureStalled);
}

TEST_CASE("new frame arrival resets the stall clock")
{
    auto const base = instant(0);
    auto tracker = uf::controller_detail::StallTracker{
        uf::MonotonicInstant::Duration{1'000},
        base
    };
    tracker.onFrameArrived(instant(900));

    CHECK(tracker.check(instant(1'900)).has_value());
    auto const result = tracker.check(instant(1'901));
    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::CaptureStalled);
}
