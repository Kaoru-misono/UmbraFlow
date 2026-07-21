#include <controller/detail/input-revalidation.hpp>
#include <controller/input.hpp>

#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace
{
    constexpr auto sessionValue = std::uint64_t{1};

    [[nodiscard]]
    auto automationKind(uf::Error const& error) -> uf::AutomationErrorKind
    {
        auto const kind = uf::automationErrorKind(error);
        if (!kind.has_value())
        {
            FAIL("The error did not contain an automation error kind");
            return uf::AutomationErrorKind::InternalInvariant;
        }
        return *kind;
    }

    [[nodiscard]]
    auto leaseAt(
        uf::TargetGeneration generation,
        uf::MonotonicInstant capturedAt
    ) -> uf::ObservationLease
    {
        auto const transform = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            4.0F,
            2.0F,
            4,
            2
        );
        REQUIRE(transform.has_value());
        auto const pixels = std::make_shared<uf::FrameBuffer const>(
            std::vector<std::byte>(32)
        );
        auto const frame = uf::Frame::create(
            uf::FrameId{1},
            uf::SessionId{sessionValue},
            generation,
            capturedAt,
            4,
            2,
            16,
            uf::PixelFormat::Bgra8,
            pixels,
            *transform
        );
        REQUIRE(frame.has_value());
        auto const lease = uf::ObservationLease::forFrame(
            *frame,
            uf::g_defaultMaxActionFrameAge
        );
        REQUIRE(lease.has_value());
        return *lease;
    }

    [[nodiscard]]
    auto instantAt(
        uf::MonotonicInstant::Duration duration
    ) -> uf::MonotonicInstant
    {
        return uf::MonotonicInstant::fromTimePoint(
            uf::MonotonicInstant::TimePoint{duration}
        );
    }

    [[nodiscard]]
    auto nextGeneration(uf::TargetGeneration generation) -> uf::TargetGeneration
    {
        auto const next = generation.next();
        REQUIRE(next.has_value());
        return *next;
    }

    [[nodiscard]]
    auto after(
        uf::MonotonicInstant instant,
        uf::MonotonicInstant::Duration duration
    ) -> uf::MonotonicInstant
    {
        auto const result = instant.checkedAdd(duration);
        REQUIRE(result.has_value());
        return *result;
    }
}

TEST_CASE("valid pointer action returns a floored pixel")
{
    auto const now = instantAt(uf::MonotonicInstant::Duration{10});
    auto const generation = uf::TargetGeneration{};
    auto const lease = leaseAt(generation, now);
    auto const result = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue},
        generation,
        now,
        uf::Point<uf::ClientSpace>{12.9F, 7.1F},
        800,
        450
    );

    REQUIRE(result.has_value());
    auto const expected = uf::ClientPixel::create(12, 7);
    REQUIRE(expected.has_value());
    CHECK(*result == *expected);
}

TEST_CASE("session mismatch is a stale observation")
{
    auto const now = instantAt(uf::MonotonicInstant::Duration{10});
    auto const generation = uf::TargetGeneration{};
    auto const lease = leaseAt(generation, now);
    auto const result = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue + 1U},
        generation,
        now,
        uf::Point<uf::ClientSpace>{1.0F, 1.0F},
        800,
        450
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::StaleObservation);
}

TEST_CASE("expired leases and generation changes are stale observations")
{
    auto const captured = instantAt(uf::MonotonicInstant::Duration{10});
    auto const generation = uf::TargetGeneration{};
    auto const lease = leaseAt(generation, captured);
    auto const expiredNow = after(
        lease.expiresAt(),
        uf::MonotonicInstant::Duration{1}
    );

    struct StaleCase final
    {
        std::string_view m_label;
        uf::TargetGeneration m_generation;
        uf::MonotonicInstant m_now;
    };
    for (auto const& testCase : std::array{
        StaleCase{"expired lease", generation, expiredNow},
        StaleCase{"generation bumped", nextGeneration(generation), captured},
    })
    {
        INFO(testCase.m_label);
        auto const result = uf::controller_detail::checkPointerPreconditions(
            lease,
            uf::SessionId{sessionValue},
            testCase.m_generation,
            testCase.m_now,
            uf::Point<uf::ClientSpace>{1.0F, 1.0F},
            800,
            450
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == uf::AutomationErrorKind::StaleObservation);
    }
}

TEST_CASE("out-of-bounds and non-finite points are action rejected")
{
    auto const now = instantAt(uf::MonotonicInstant::Duration{10});
    auto const generation = uf::TargetGeneration{};
    auto const lease = leaseAt(generation, now);
    struct InvalidPoint final
    {
        std::string_view m_label;
        uf::Point<uf::ClientSpace> m_point;
    };
    for (auto const& testCase : std::array{
        InvalidPoint{"negative x", {-0.1F, 10.0F}},
        InvalidPoint{"negative y", {10.0F, -0.1F}},
        InvalidPoint{"right edge excluded", {800.0F, 10.0F}},
        InvalidPoint{"bottom edge excluded", {10.0F, 450.0F}},
        InvalidPoint{
            "non-finite x",
            {std::numeric_limits<float>::quiet_NaN(), 10.0F}
        },
        InvalidPoint{
            "non-finite y",
            {10.0F, std::numeric_limits<float>::infinity()}
        },
    })
    {
        INFO(testCase.m_label);
        auto const result = uf::controller_detail::checkPointerPreconditions(
            lease,
            uf::SessionId{sessionValue},
            generation,
            now,
            testCase.m_point,
            800,
            450
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == uf::AutomationErrorKind::ActionRejected);
    }
}

TEST_CASE("pointer coordinates must fit signed sixteen bits")
{
    auto const now = instantAt(uf::MonotonicInstant::Duration{10});
    auto const generation = uf::TargetGeneration{};
    auto const lease = leaseAt(generation, now);
    auto const accepted = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue},
        generation,
        now,
        uf::Point<uf::ClientSpace>{32'767.9F, 1.0F},
        40'000,
        450
    );
    REQUIRE(accepted.has_value());
    auto const expected = uf::ClientPixel::create(32'767, 1);
    REQUIRE(expected.has_value());
    CHECK(*accepted == *expected);

    auto const rejected = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue},
        generation,
        now,
        uf::Point<uf::ClientSpace>{32'768.0F, 1.0F},
        40'000,
        450
    );
    REQUIRE_FALSE(rejected.has_value());
    CHECK(automationKind(rejected.error()) == uf::AutomationErrorKind::ActionRejected);
}

TEST_CASE("keyboard generation mismatch is a stale observation")
{
    auto const generation = uf::TargetGeneration{};
    CHECK(uf::controller_detail::checkKeyboardPreconditions(generation, generation));
    auto const result = uf::controller_detail::checkKeyboardPreconditions(
        generation,
        nextGeneration(generation)
    );
    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::StaleObservation);
}

TEST_CASE("bogus handles are not alive and are rejected")
{
    auto const bogus = uf::WindowHandle{std::intptr_t{0xDEAD'BEEF}};
    CHECK_FALSE(uf::controller_detail::windowIsAlive(bogus));
    auto const result = uf::controller_detail::ensureWindowAlive(bogus);
    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::ActionRejected);
}

TEST_CASE("pointer preconditions preserve lease generation bounds order")
{
    auto const captured = instantAt(uf::MonotonicInstant::Duration{10});
    auto const generation = uf::TargetGeneration{};
    auto const lease = leaseAt(generation, captured);
    auto const bumped = nextGeneration(generation);
    auto const expired = after(lease.expiresAt(), uf::MonotonicInstant::Duration{1});
    auto const outside = uf::Point<uf::ClientSpace>{-1.0F, -1.0F};

    auto const sessionFailure = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue + 1U},
        bumped,
        expired,
        outside,
        800,
        450
    );
    REQUIRE_FALSE(sessionFailure.has_value());
    CHECK(sessionFailure.error().message().contains("lease session"));

    auto const expiryFailure = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue},
        bumped,
        expired,
        outside,
        800,
        450
    );
    REQUIRE_FALSE(expiryFailure.has_value());
    CHECK(expiryFailure.error().message().contains("lease expired"));

    auto const generationFailure = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue},
        bumped,
        captured,
        outside,
        800,
        450
    );
    REQUIRE_FALSE(generationFailure.has_value());
    CHECK(generationFailure.error().message().contains("lease generation"));

    auto const boundsFailure = uf::controller_detail::checkPointerPreconditions(
        lease,
        uf::SessionId{sessionValue},
        generation,
        captured,
        outside,
        800,
        450
    );
    REQUIRE_FALSE(boundsFailure.has_value());
    CHECK(boundsFailure.error().message().contains("outside client area"));
}
