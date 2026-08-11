#include <domain/detection.hpp>

#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <vector>

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

        // The deadline a lease over a live target carries. Only forRecordedFrame
        // produces one without, and its own case below reads the optional
        // directly rather than through this.
        [[nodiscard]]
        auto deadlineOf(ObservationLease const& lease) -> MonotonicInstant
        {
            auto const expiresAt = lease.expiresAt();
            REQUIRE(expiresAt.has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            return *expiresAt;
        }

        auto makeFrame(
            uint64 session,
            TargetGeneration generation,
            uint64 id,
            MonotonicInstant capturedAt
        ) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                2.0F,
                2.0F,
                2,
                2
            );
            REQUIRE(transform.has_value());

            auto const pixels = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(
                    std::vector<std::byte>(16)
                )
            };
            auto const frame = Frame::create(
                FrameId{id},
                CaptureSessionId{session},
                generation,
                capturedAt,
                2,
                2,
                8,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            return *frame;
        }

        auto addTime(
            MonotonicInstant instant,
            MonotonicInstant::Duration duration
        ) -> MonotonicInstant
        {
            auto const result = instant.checkedAdd(duration);
            REQUIRE(result.has_value());
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): REQUIRE above proved engagement.
            return *result;
        }

        auto requireStaleObservation(Status const& result) -> void
        {
            REQUIRE_FALSE(result.has_value());
            auto const kind = automationErrorKind(result.error());
            REQUIRE(kind.has_value());
            CHECK(kind == AutomationErrorKind::StaleObservation);
        }
    }

    TEST_CASE("maximum action frame age can only be shortened")
    {
        CHECK(
            clampMaxActionFrameAge(clockDuration(std::chrono::seconds{5}))
            == k_defaultMaxActionFrameAge
        );
        CHECK(
            clampMaxActionFrameAge(clockDuration(std::chrono::milliseconds{100}))
            == clockDuration(std::chrono::milliseconds{100})
        );
    }

    TEST_CASE("observation lease expires strictly after its deadline")
    {
        using Duration = MonotonicInstant::Duration;

        auto const capturedAt = instantAt(Duration{10});
        auto const frame = makeFrame(1, TargetGeneration{}, 10, capturedAt);
        auto const lease = ObservationLease::forFrame(
            frame,
            Duration{2}
        );

        REQUIRE(lease.has_value());
        auto const beforeDeadline = addTime(capturedAt, Duration{1});
        CHECK_FALSE(lease->isExpired(beforeDeadline));
        CHECK_FALSE(lease->isExpired(deadlineOf(*lease)));
        auto const afterDeadline = addTime(
            deadlineOf(*lease),
            Duration{1}
        );
        CHECK(lease->isExpired(afterDeadline));

        auto const zeroLease = ObservationLease::forFrame(
            frame,
            Duration::zero()
        );
        REQUIRE(zeroLease.has_value());
        CHECK(deadlineOf(*zeroLease) == capturedAt);
        CHECK_FALSE(zeroLease->isExpired(capturedAt));
        CHECK(zeroLease->isExpired(addTime(capturedAt, Duration{1})));
    }

    TEST_CASE("observation lease binds frame identity and clamps its age")
    {
        auto const generation = TargetGeneration::fromValue(7);
        auto const frame = makeFrame(
            3,
            generation,
            11,
            instantAt(MonotonicInstant::Duration{1})
        );
        auto const lease = ObservationLease::forFrame(
            frame,
            clockDuration(std::chrono::seconds{5})
        );

        REQUIRE(lease.has_value());
        CHECK(lease->sessionId() == frame.sessionId());
        CHECK(lease->targetGeneration() == frame.targetGeneration());
        CHECK(lease->frameId() == frame.id());
        CHECK(
            deadlineOf(*lease).saturatingDurationSince(frame.capturedAt())
            == k_defaultMaxActionFrameAge
        );
    }

    TEST_CASE("observation validation rejects every identity mismatch and expiry")
    {
        auto const generation = TargetGeneration{};
        auto const frame = makeFrame(
            1,
            generation,
            10,
            instantAt(MonotonicInstant::Duration{1})
        );
        auto const lease = ObservationLease::forFrame(
            frame,
            k_defaultMaxActionFrameAge
        );
        REQUIRE(lease.has_value());

        CHECK(
            lease->validate(
                CaptureSessionId{uint64{1}},
                generation,
                FrameId{uint64{10}},
                deadlineOf(*lease)
            )
        );

        requireStaleObservation(
            lease->validate(
                CaptureSessionId{uint64{2}},
                generation,
                FrameId{uint64{10}},
                frame.capturedAt()
            )
        );

        auto const nextGeneration = generation.next();
        REQUIRE(nextGeneration.has_value());
        requireStaleObservation(
            lease->validate(
                CaptureSessionId{uint64{1}},
                *nextGeneration,
                FrameId{uint64{10}},
                frame.capturedAt()
            )
        );

        requireStaleObservation(
            lease->validate(
                CaptureSessionId{uint64{1}},
                generation,
                FrameId{uint64{11}},
                frame.capturedAt()
            )
        );

        requireStaleObservation(
            lease->validate(
                CaptureSessionId{uint64{1}},
                generation,
                FrameId{uint64{10}},
                addTime(
                    deadlineOf(*lease),
                    MonotonicInstant::Duration{1}
                )
            )
        );
    }

    TEST_CASE("observation lease deadline overflow uses the exact boundary")
    {
        using Duration = MonotonicInstant::Duration;

        auto const lastValidCapture = Duration::max() - k_defaultMaxActionFrameAge;
        auto const validFrame = makeFrame(
            1,
            TargetGeneration{},
            1,
            instantAt(lastValidCapture)
        );
        auto const validLease = ObservationLease::forFrame(
            validFrame,
            k_defaultMaxActionFrameAge
        );
        REQUIRE(validLease.has_value());
        CHECK(
            deadlineOf(*validLease).timePoint().time_since_epoch()
            == Duration::max()
        );

        auto const overflowFrame = makeFrame(
            1,
            TargetGeneration{},
            2,
            instantAt(lastValidCapture + Duration{1})
        );
        auto const overflowLease = ObservationLease::forFrame(
            overflowFrame,
            k_defaultMaxActionFrameAge
        );
        REQUIRE_FALSE(overflowLease.has_value());
        auto const kind = automationErrorKind(overflowLease.error());
        REQUIRE(kind.has_value());
        CHECK(kind == AutomationErrorKind::InternalInvariant);
    }

    // A lease over recorded bytes keeps every identity clause and drops only the
    // deadline, because the interval it would measure is the observer's and not
    // the target's. The clock is walked past where a live lease of the same
    // frame would have died, which is the comparison that makes this a property
    // rather than a restatement of the code.
    TEST_CASE("a recorded lease refuses on identity and never on age")
    {
        auto const generation = TargetGeneration::fromValue(5);
        auto const capturedAt = instantAt(MonotonicInstant::Duration{1});
        auto const frame = makeFrame(4, generation, 12, capturedAt);

        auto const liveLease = ObservationLease::forFrame(
            frame,
            k_defaultMaxActionFrameAge
        );
        REQUIRE(liveLease.has_value());
        auto const wellPastLive = addTime(
            deadlineOf(*liveLease),
            clockDuration(std::chrono::hours{1})
        );

        auto const lease = ObservationLease::forRecordedFrame(frame);
        CHECK(lease.sessionId() == frame.sessionId());
        CHECK(lease.targetGeneration() == frame.targetGeneration());
        CHECK(lease.frameId() == frame.id());
        CHECK_FALSE(lease.expiresAt().has_value());
        CHECK_FALSE(lease.isExpired(wellPastLive));
        CHECK(liveLease->isExpired(wellPastLive));

        CHECK(
            lease.validate(
                frame.sessionId(),
                generation,
                frame.id(),
                wellPastLive
            )
        );

        auto const nextGeneration = generation.next();
        REQUIRE(nextGeneration.has_value());
        requireStaleObservation(
            lease.validate(
                frame.sessionId(),
                *nextGeneration,
                frame.id(),
                capturedAt
            )
        );
        requireStaleObservation(
            lease.validate(
                CaptureSessionId{uint64{99}},
                generation,
                frame.id(),
                capturedAt
            )
        );
        requireStaleObservation(
            lease.validate(
                frame.sessionId(),
                generation,
                FrameId{uint64{13}},
                capturedAt
            )
        );
    }

    TEST_CASE("negative observation ages fail closed")
    {
        auto const frame = makeFrame(
            1,
            TargetGeneration{},
            1,
            instantAt(MonotonicInstant::Duration{1})
        );
        auto const lease = ObservationLease::forFrame(
            frame,
            MonotonicInstant::Duration{-1}
        );

        REQUIRE_FALSE(lease.has_value());
        auto const kind = automationErrorKind(lease.error());
        REQUIRE(kind.has_value());
        CHECK(kind == AutomationErrorKind::InternalInvariant);
    }
}
