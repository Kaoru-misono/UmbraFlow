#include <controller/detail/capture-stall.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <string>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto automationKind(Error const& error) -> AutomationErrorKind
        {
            auto const kind = automationErrorKind(error);
            if (!kind.has_value())
            {
                FAIL("The error did not contain an automation error kind");
                return AutomationErrorKind::InternalInvariant;
            }
            return *kind;
        }

        [[nodiscard]]
        auto instant(
            MonotonicInstant::Duration::rep ticks
        ) -> MonotonicInstant
        {
            return MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{MonotonicInstant::Duration{ticks}}
            );
        }

        [[nodiscard]]
        auto plus(
            MonotonicInstant base,
            MonotonicInstant::Duration duration
        ) -> MonotonicInstant
        {
            auto const added = base.checkedAdd(duration);
            REQUIRE(added.has_value());
            return *added;
        }

        [[nodiscard]]
        auto stallMessage(
            controller_detail::TargetWindowState observed
        ) -> std::string
        {
            auto const failure = controller_detail::stalledFrameFailure(
                MonotonicInstant::Duration{1'000},
                observed
            );
            CHECK(automationKind(failure.error()) == AutomationErrorKind::CaptureStalled);
            return std::string{failure.error().message()};
        }
    }

    TEST_CASE("stall timeout accepts the exact boundary and rejects the next tick")
    {
        auto const base = instant(10'000);
        auto const timeout = MonotonicInstant::Duration{1'000};
        auto const tracker = controller_detail::StallTracker{
            timeout,
            base
        };

        CHECK(
            tracker
                .check(
                    plus(base, timeout),
                    controller_detail::TargetWindowState::Composing
                )
                .has_value()
        );
        auto const result = tracker.check(
            plus(base, timeout + MonotonicInstant::Duration{1}),
            controller_detail::TargetWindowState::Composing
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::CaptureStalled);
    }

    TEST_CASE("frame stranded in the slot is stale by arrival rather than consumption")
    {
        auto const timeout = MonotonicInstant::Duration{1'000};
        auto tracker = controller_detail::StallTracker{
            timeout,
            instant(0)
        };
        tracker.onFrameArrived(instant(100));

        CHECK(
            tracker
                .check(
                    instant(1'100),
                    controller_detail::TargetWindowState::Composing
                )
                .has_value()
        );
        auto const result = tracker.check(
            instant(1'101),
            controller_detail::TargetWindowState::Composing
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::CaptureStalled);
    }

    TEST_CASE("new frame arrival resets the stall clock")
    {
        auto const base = instant(0);
        auto tracker = controller_detail::StallTracker{
            MonotonicInstant::Duration{1'000},
            base
        };
        tracker.onFrameArrived(instant(900));

        CHECK(
            tracker
                .check(
                    instant(1'900),
                    controller_detail::TargetWindowState::Composing
                )
                .has_value()
        );
        auto const result = tracker.check(
            instant(1'901),
            controller_detail::TargetWindowState::Composing
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::CaptureStalled);
    }

    // The incident this pins: a minimized target stalled every capture and the
    // message named only the symptom, so the operator hunted the capture backend
    // instead of restoring a window. Naming the state is not enough on its own —
    // the message has to carry the action too.
    TEST_CASE("a stall behind a minimized window names the state and the fix")
    {
        auto const message = stallMessage(
            controller_detail::TargetWindowState::Minimized
        );

        // "window is minimized" rather than "minimized", because the message for
        // a window that is fine says "is not minimized" and would satisfy the
        // looser check while explaining nothing.
        CHECK(message.contains("window is minimized"));
        CHECK(message.contains("Restore the window"));
        CHECK(message.contains("1000 monotonic clock ticks"));
    }

    TEST_CASE("a stall behind a destroyed window names the state and the fix")
    {
        auto const message = stallMessage(
            controller_detail::TargetWindowState::Destroyed
        );

        CHECK(message.contains("no longer exists"));
        CHECK(message.contains("Resolve the target again"));
    }

    // The inverse matters as much: a stall with nothing observably wrong must not
    // borrow the minimized explanation, or the next operator restores a window
    // that was never minimized and learns to distrust the message.
    TEST_CASE("a stall with no observable cause says so instead of guessing")
    {
        auto const message = stallMessage(
            controller_detail::TargetWindowState::Composing
        );

        CHECK(message.contains("does not explain the stall"));
        CHECK_FALSE(message.contains("Restore the window"));
        CHECK_FALSE(message.contains("no longer exists"));
    }

    // The tracker is what actually fires on the capture path, so the observation
    // has to survive the trip through it rather than only through the formatter.
    TEST_CASE("the tracker reports the window state it was given")
    {
        auto const base = instant(0);
        auto const tracker = controller_detail::StallTracker{
            MonotonicInstant::Duration{1'000},
            base
        };

        auto const result = tracker.check(
            instant(1'001),
            controller_detail::TargetWindowState::Minimized
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::CaptureStalled);
        CHECK(result.error().message().contains("window is minimized"));
        CHECK(result.error().message().contains("Restore the window"));
    }
}
