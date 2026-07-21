#include <controller/detail/input-revalidation.hpp>
#include <controller/input.hpp>

#include <core/types/integer.hpp>
#include <domain/detection.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace uf
{
    namespace
    {
        constexpr auto g_sessionValue = uint64{1};

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
        auto leaseAt(
            TargetGeneration generation,
            MonotonicInstant capturedAt
        ) -> ObservationLease
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                4.0F,
                2.0F,
                4,
                2
            );
            REQUIRE(transform.has_value());
            auto const pixels = std::make_shared<FrameBuffer const>(
                std::vector<std::byte>(32)
            );
            auto const frame = Frame::create(
                FrameId{1},
                SessionId{g_sessionValue},
                generation,
                capturedAt,
                4,
                2,
                16,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            auto const lease = ObservationLease::forFrame(
                *frame,
                g_defaultMaxActionFrameAge
            );
            REQUIRE(lease.has_value());
            return *lease;
        }

        [[nodiscard]]
        auto instantAt(
            MonotonicInstant::Duration duration
        ) -> MonotonicInstant
        {
            return MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{duration}
            );
        }

        [[nodiscard]]
        auto nextGeneration(TargetGeneration generation) -> TargetGeneration
        {
            auto const next = generation.next();
            REQUIRE(next.has_value());
            return *next;
        }

        [[nodiscard]]
        auto after(
            MonotonicInstant instant,
            MonotonicInstant::Duration duration
        ) -> MonotonicInstant
        {
            auto const result = instant.checkedAdd(duration);
            REQUIRE(result.has_value());
            return *result;
        }
    }

    TEST_CASE("valid pointer action returns a floored pixel")
    {
        auto const now = instantAt(MonotonicInstant::Duration{10});
        auto const generation = TargetGeneration{};
        auto const lease = leaseAt(generation, now);
        auto const result = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue},
            generation,
            now,
            Point<ClientSpace>{12.9F, 7.1F},
            800,
            450
        );

        REQUIRE(result.has_value());
        auto const expected = ClientPixel::create(12, 7);
        REQUIRE(expected.has_value());
        CHECK(*result == *expected);
    }

    TEST_CASE("session mismatch is a stale observation")
    {
        auto const now = instantAt(MonotonicInstant::Duration{10});
        auto const generation = TargetGeneration{};
        auto const lease = leaseAt(generation, now);
        auto const result = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue + 1U},
            generation,
            now,
            Point<ClientSpace>{1.0F, 1.0F},
            800,
            450
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::StaleObservation);
    }

    TEST_CASE("expired leases and generation changes are stale observations")
    {
        auto const captured = instantAt(MonotonicInstant::Duration{10});
        auto const generation = TargetGeneration{};
        auto const lease = leaseAt(generation, captured);
        auto const expiredNow = after(
            lease.expiresAt(),
            MonotonicInstant::Duration{1}
        );

        struct StaleCase final
        {
            std::string_view m_label;
            TargetGeneration m_generation;
            MonotonicInstant m_now;
        };
        for (auto const& testCase : std::array{
            StaleCase{"expired lease", generation, expiredNow},
            StaleCase{"generation bumped", nextGeneration(generation), captured},
        })
        {
            INFO(testCase.m_label);
            auto const result = controller_detail::checkPointerPreconditions(
                lease,
                SessionId{g_sessionValue},
                testCase.m_generation,
                testCase.m_now,
                Point<ClientSpace>{1.0F, 1.0F},
                800,
                450
            );
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::StaleObservation);
        }
    }

    TEST_CASE("out-of-bounds and non-finite points are action rejected")
    {
        auto const now = instantAt(MonotonicInstant::Duration{10});
        auto const generation = TargetGeneration{};
        auto const lease = leaseAt(generation, now);
        struct InvalidPoint final
        {
            std::string_view m_label;
            Point<ClientSpace> m_point;
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
            auto const result = controller_detail::checkPointerPreconditions(
                lease,
                SessionId{g_sessionValue},
                generation,
                now,
                testCase.m_point,
                800,
                450
            );
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::ActionRejected);
        }
    }

    TEST_CASE("pointer coordinates must fit signed sixteen bits")
    {
        auto const now = instantAt(MonotonicInstant::Duration{10});
        auto const generation = TargetGeneration{};
        auto const lease = leaseAt(generation, now);
        auto const accepted = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue},
            generation,
            now,
            Point<ClientSpace>{32'767.9F, 1.0F},
            40'000,
            450
        );
        REQUIRE(accepted.has_value());
        auto const expected = ClientPixel::create(32'767, 1);
        REQUIRE(expected.has_value());
        CHECK(*accepted == *expected);

        auto const rejected = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue},
            generation,
            now,
            Point<ClientSpace>{32'768.0F, 1.0F},
            40'000,
            450
        );
        REQUIRE_FALSE(rejected.has_value());
        CHECK(automationKind(rejected.error()) == AutomationErrorKind::ActionRejected);
    }

    TEST_CASE("keyboard generation mismatch is a stale observation")
    {
        auto const generation = TargetGeneration{};
        CHECK(controller_detail::checkKeyboardPreconditions(generation, generation));
        auto const result = controller_detail::checkKeyboardPreconditions(
            generation,
            nextGeneration(generation)
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::StaleObservation);
    }

    TEST_CASE("bogus handles are not alive and are rejected")
    {
        auto const bogus = WindowHandle{intptr{0xDEAD'BEEF}};
        CHECK_FALSE(controller_detail::windowIsAlive(bogus));
        auto const result = controller_detail::ensureWindowAlive(bogus);
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::ActionRejected);
    }

    TEST_CASE("pointer preconditions preserve lease generation bounds order")
    {
        auto const captured = instantAt(MonotonicInstant::Duration{10});
        auto const generation = TargetGeneration{};
        auto const lease = leaseAt(generation, captured);
        auto const bumped = nextGeneration(generation);
        auto const expired = after(lease.expiresAt(), MonotonicInstant::Duration{1});
        auto const outside = Point<ClientSpace>{-1.0F, -1.0F};

        auto const sessionFailure = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue + 1U},
            bumped,
            expired,
            outside,
            800,
            450
        );
        REQUIRE_FALSE(sessionFailure.has_value());
        CHECK(sessionFailure.error().message().contains("lease session"));

        auto const expiryFailure = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue},
            bumped,
            expired,
            outside,
            800,
            450
        );
        REQUIRE_FALSE(expiryFailure.has_value());
        CHECK(expiryFailure.error().message().contains("lease expired"));

        auto const generationFailure = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue},
            bumped,
            captured,
            outside,
            800,
            450
        );
        REQUIRE_FALSE(generationFailure.has_value());
        CHECK(generationFailure.error().message().contains("lease generation"));

        auto const boundsFailure = controller_detail::checkPointerPreconditions(
            lease,
            SessionId{g_sessionValue},
            generation,
            captured,
            outside,
            800,
            450
        );
        REQUIRE_FALSE(boundsFailure.has_value());
        CHECK(boundsFailure.error().message().contains("outside client area"));
    }
}
