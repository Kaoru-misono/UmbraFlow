#include <domain/detection.hpp>

#include <doctest/doctest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

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

    auto makeFrame(
        std::uint64_t session,
        uf::TargetGeneration generation,
        std::uint64_t id,
        uf::MonotonicInstant capturedAt
    ) -> uf::Frame
    {
        auto const transform = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            2.0F,
            2.0F,
            2,
            2
        );
        REQUIRE(transform.has_value());

        auto const pixels = std::shared_ptr<uf::FrameBuffer const>{
            std::make_shared<uf::FrameBuffer>(
                std::vector<std::byte>(16)
            )
        };
        auto const frame = uf::Frame::create(
            uf::FrameId{id},
            uf::SessionId{session},
            generation,
            capturedAt,
            2,
            2,
            8,
            uf::PixelFormat::Bgra8,
            pixels,
            *transform
        );
        REQUIRE(frame.has_value());
        return *frame;
    }

    auto addTime(
        uf::MonotonicInstant instant,
        uf::MonotonicInstant::Duration duration
    ) -> uf::MonotonicInstant
    {
        auto const result = uf::checkedAddMonotonic(instant, duration);
        REQUIRE(result.has_value());
        return *result;
    }

    auto requireStaleObservation(uf::Status const& result) -> void
    {
        REQUIRE_FALSE(result.has_value());
        auto const kind = uf::automationErrorKind(result.error());
        REQUIRE(kind.has_value());
        CHECK(*kind == uf::AutomationErrorKind::StaleObservation);
    }
}

TEST_CASE("maximum action frame age can only be shortened")
{
    CHECK(
        uf::clampMaxActionFrameAge(clockDuration(std::chrono::seconds{5}))
        == uf::g_defaultMaxActionFrameAge
    );
    CHECK(
        uf::clampMaxActionFrameAge(clockDuration(std::chrono::milliseconds{100}))
        == clockDuration(std::chrono::milliseconds{100})
    );
}

TEST_CASE("observation lease expires strictly after its deadline")
{
    using Duration = uf::MonotonicInstant::Duration;

    auto const capturedAt = instantAt(Duration{10});
    auto const frame = makeFrame(1, uf::TargetGeneration{}, 10, capturedAt);
    auto const lease = uf::ObservationLease::forFrame(
        frame,
        Duration{2}
    );

    REQUIRE(lease.has_value());
    auto const beforeDeadline = addTime(capturedAt, Duration{1});
    CHECK_FALSE(lease->isExpired(beforeDeadline));
    CHECK_FALSE(lease->isExpired(lease->expiresAt()));
    auto const afterDeadline = addTime(
        lease->expiresAt(),
        Duration{1}
    );
    CHECK(lease->isExpired(afterDeadline));

    auto const zeroLease = uf::ObservationLease::forFrame(
        frame,
        Duration::zero()
    );
    REQUIRE(zeroLease.has_value());
    CHECK(zeroLease->expiresAt() == capturedAt);
    CHECK_FALSE(zeroLease->isExpired(capturedAt));
    CHECK(zeroLease->isExpired(addTime(capturedAt, Duration{1})));
}

TEST_CASE("observation lease binds frame identity and clamps its age")
{
    auto const generation = uf::TargetGeneration::fromValue(7);
    auto const frame = makeFrame(
        3,
        generation,
        11,
        instantAt(uf::MonotonicInstant::Duration{1})
    );
    auto const lease = uf::ObservationLease::forFrame(
        frame,
        clockDuration(std::chrono::seconds{5})
    );

    REQUIRE(lease.has_value());
    CHECK(lease->sessionId() == frame.sessionId());
    CHECK(lease->targetGeneration() == frame.targetGeneration());
    CHECK(lease->frameId() == frame.id());
    CHECK(
        lease->expiresAt().saturatingDurationSince(frame.capturedAt())
        == uf::g_defaultMaxActionFrameAge
    );
}

TEST_CASE("observation validation rejects every identity mismatch and expiry")
{
    auto const generation = uf::TargetGeneration{};
    auto const frame = makeFrame(
        1,
        generation,
        10,
        instantAt(uf::MonotonicInstant::Duration{1})
    );
    auto const lease = uf::ObservationLease::forFrame(
        frame,
        uf::g_defaultMaxActionFrameAge
    );
    REQUIRE(lease.has_value());

    CHECK(
        lease->validate(
            uf::SessionId{std::uint64_t{1}},
            generation,
            uf::FrameId{std::uint64_t{10}},
            lease->expiresAt()
        )
    );

    requireStaleObservation(
        lease->validate(
            uf::SessionId{std::uint64_t{2}},
            generation,
            uf::FrameId{std::uint64_t{10}},
            frame.capturedAt()
        )
    );

    auto const nextGeneration = generation.next();
    REQUIRE(nextGeneration.has_value());
    requireStaleObservation(
        lease->validate(
            uf::SessionId{std::uint64_t{1}},
            *nextGeneration,
            uf::FrameId{std::uint64_t{10}},
            frame.capturedAt()
        )
    );

    requireStaleObservation(
        lease->validate(
            uf::SessionId{std::uint64_t{1}},
            generation,
            uf::FrameId{std::uint64_t{11}},
            frame.capturedAt()
        )
    );

    requireStaleObservation(
        lease->validate(
            uf::SessionId{std::uint64_t{1}},
            generation,
            uf::FrameId{std::uint64_t{10}},
            addTime(
                lease->expiresAt(),
                uf::MonotonicInstant::Duration{1}
            )
        )
    );
}

TEST_CASE("observation lease deadline overflow uses the exact boundary")
{
    using Duration = uf::MonotonicInstant::Duration;

    auto const lastValidCapture = Duration::max() - uf::g_defaultMaxActionFrameAge;
    auto const validFrame = makeFrame(
        1,
        uf::TargetGeneration{},
        1,
        instantAt(lastValidCapture)
    );
    auto const validLease = uf::ObservationLease::forFrame(
        validFrame,
        uf::g_defaultMaxActionFrameAge
    );
    REQUIRE(validLease.has_value());
    CHECK(
        validLease->expiresAt().timePoint().time_since_epoch()
        == Duration::max()
    );

    auto const overflowFrame = makeFrame(
        1,
        uf::TargetGeneration{},
        2,
        instantAt(lastValidCapture + Duration{1})
    );
    auto const overflowLease = uf::ObservationLease::forFrame(
        overflowFrame,
        uf::g_defaultMaxActionFrameAge
    );
    REQUIRE_FALSE(overflowLease.has_value());
    auto const kind = uf::automationErrorKind(overflowLease.error());
    REQUIRE(kind.has_value());
    CHECK(*kind == uf::AutomationErrorKind::InternalInvariant);
}

TEST_CASE("negative observation ages fail closed")
{
    auto const frame = makeFrame(
        1,
        uf::TargetGeneration{},
        1,
        instantAt(uf::MonotonicInstant::Duration{1})
    );
    auto const lease = uf::ObservationLease::forFrame(
        frame,
        uf::MonotonicInstant::Duration{-1}
    );

    REQUIRE_FALSE(lease.has_value());
    auto const kind = uf::automationErrorKind(lease.error());
    REQUIRE(kind.has_value());
    CHECK(*kind == uf::AutomationErrorKind::InternalInvariant);
}
