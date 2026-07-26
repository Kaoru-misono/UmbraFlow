#include <controller/capture.hpp>
#include <controller/detail/capture-wgc.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <limits>
#include <utility>

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
        auto geometry(
            float originX,
            float originY,
            float width,
            float height
        ) -> Result<ClientGeometry>
        {
            return ClientGeometry::create(
                Point<DesktopSpace>{originX, originY},
                width,
                height
            );
        }
    }

    TEST_CASE("frame ids increase monotonically within a session")
    {
        auto counter = controller_detail::FrameIdCounter{};
        auto const first = counter.nextId();
        auto const second = counter.nextId();
        auto const third = counter.nextId();

        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        REQUIRE(third.has_value());
        CHECK(first->value() < second->value());
        CHECK(second->value() < third->value());
        CHECK(second->value() == first->value() + 1);
    }

    TEST_CASE("frame id counter overflow is rejected")
    {
        auto counter = controller_detail::FrameIdCounter{
            std::numeric_limits<uint64>::max()
        };
        auto const result = counter.nextId();

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::InternalInvariant);
    }

    TEST_CASE("client geometry rejects non-finite and non-positive values")
    {
        struct Case final
        {
            float originX{};
            float originY{};
            float width{};
            float height{};
        };

        for (
            auto const& testCase : std::array{
                Case{std::numeric_limits<float>::quiet_NaN(), 0.0F, 100.0F, 100.0F},
                Case{0.0F, std::numeric_limits<float>::infinity(), 100.0F, 100.0F},
                Case{0.0F, 0.0F, 0.0F, 100.0F},
                Case{0.0F, 0.0F, 100.0F, -1.0F},
            }
        )
        {
            CAPTURE(testCase.originX);
            CAPTURE(testCase.originY);
            CAPTURE(testCase.width);
            CAPTURE(testCase.height);
            auto const result = geometry(
                testCase.originX,
                testCase.originY,
                testCase.width,
                testCase.height
            );
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationKind(result.error())
                == AutomationErrorKind::InternalInvariant
            );
        }
    }

    TEST_CASE("client geometry builds a transform with the captured frame extent")
    {
        auto const client = geometry(100.0F, 50.0F, 1'600.0F, 900.0F);
        REQUIRE(client.has_value());
        auto const transform = client->transformFor(800, 450);

        REQUIRE(transform.has_value());
        CHECK(transform->frameSize() == std::pair{uint32{800}, uint32{450}});
        CHECK(
            transform->desktopToFrame(Point<DesktopSpace>{100.0F, 50.0F})
            == Point<FrameSpace>{0.0F, 0.0F}
        );
        CHECK(
            transform->desktopToFrame(Point<DesktopSpace>{1'700.0F, 950.0F})
            == Point<FrameSpace>{800.0F, 450.0F}
        );
    }

    TEST_CASE("client geometry integer extent requires whole pixels")
    {
        auto const whole = geometry(0.0F, 0.0F, 2'560.0F, 1'440.0F);
        REQUIRE(whole.has_value());
        auto const extent = controller_detail::clientIntegerExtent(*whole);
        REQUIRE(extent.has_value());
        CHECK(*extent == std::pair{uint32{2'560}, uint32{1'440}});

        for (
            auto const& dimensions : std::array{
                std::pair{2'560.5F, 1'440.0F},
                std::pair{2'560.0F, 1'440.25F},
            }
        )
        {
            CAPTURE(dimensions.first);
            CAPTURE(dimensions.second);
            auto const client = geometry(
                0.0F,
                0.0F,
                dimensions.first,
                dimensions.second
            );
            REQUIRE(client.has_value());
            auto const result = controller_detail::clientIntegerExtent(*client);
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationKind(result.error())
                == AutomationErrorKind::CaptureUnavailable
            );
        }
    }

    TEST_CASE("capture geometry accepts matching content and surface sizes")
    {
        auto state = controller_detail::CaptureGeometryState::create({800, 450});
        REQUIRE(state.has_value());
        auto const confirmed = state->observeContentSize({800, 450});
        REQUIRE(confirmed.has_value());

        CHECK(state->observeSurfaceSize(*confirmed, 800, 450).has_value());
        CHECK(state->ensureActive().has_value());
    }

    TEST_CASE("content size change invalidates capture geometry until rebuild")
    {
        auto state = controller_detail::CaptureGeometryState::create({800, 450});
        REQUIRE(state.has_value());
        auto const changed = state->observeContentSize({801, 450});
        REQUIRE_FALSE(changed.has_value());
        CHECK(automationKind(changed.error()) == AutomationErrorKind::CaptureUnavailable);

        auto const latched = state->observeContentSize({800, 450});
        REQUIRE_FALSE(latched.has_value());
        CHECK(automationKind(latched.error()) == AutomationErrorKind::CaptureUnavailable);
    }

    TEST_CASE("invalid content size and surface mismatch fail closed")
    {
        for (
            auto const size : std::array{
                controller_detail::CaptureSize{0, 450},
                controller_detail::CaptureSize{-1, 450},
            }
        )
        {
            CAPTURE(size.width);
            CAPTURE(size.height);
            auto state = controller_detail::CaptureGeometryState::create({800, 450});
            REQUIRE(state.has_value());
            auto const result = state->observeContentSize(size);
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationKind(result.error())
                == AutomationErrorKind::CaptureUnavailable
            );
            CHECK_FALSE(state->ensureActive().has_value());
        }

        auto state = controller_detail::CaptureGeometryState::create({800, 450});
        REQUIRE(state.has_value());
        auto const mismatch = state->observeSurfaceSize({800, 450}, 800, 451);
        REQUIRE_FALSE(mismatch.has_value());
        CHECK(
            automationKind(mismatch.error())
            == AutomationErrorKind::CaptureUnavailable
        );
        CHECK_FALSE(state->ensureActive().has_value());
    }

    TEST_CASE("capture options reject zero timeout and default to a positive second")
    {
        for (
            auto const timeout : std::array{
                MonotonicInstant::Duration::zero(),
                -MonotonicInstant::Duration{1},
            }
        )
        {
            CAPTURE(timeout.count());
            auto const invalid = WgcCaptureOptions::create(timeout, false);
            REQUIRE_FALSE(invalid.has_value());
            CHECK(
                automationKind(invalid.error())
                == AutomationErrorKind::InternalInvariant
            );
        }

        auto const options = WgcCaptureOptions{};
        CHECK(options.captureStallTimeout() == k_defaultCaptureStallTimeout);
        CHECK_FALSE(options.requireBorderless());
        CHECK(options.captureStallTimeout() > MonotonicInstant::Duration::zero());
    }
}
