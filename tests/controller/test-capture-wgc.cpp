#include <controller/capture.hpp>
#include <controller/detail/capture-wgc.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <utility>

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
    auto geometry(
        float originX,
        float originY,
        float width,
        float height
    ) -> uf::Result<uf::ClientGeometry>
    {
        return uf::ClientGeometry::create(
            uf::Point<uf::DesktopSpace>{originX, originY},
            width,
            height
        );
    }
}

TEST_CASE("frame ids increase monotonically within a session")
{
    auto counter = uf::controller_detail::FrameIdCounter{};
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
    auto counter = uf::controller_detail::FrameIdCounter{
        std::numeric_limits<std::uint64_t>::max()
    };
    auto const result = counter.nextId();

    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::InternalInvariant);
}

TEST_CASE("client geometry rejects non-finite and non-positive values")
{
    struct Case final
    {
        float m_originX;
        float m_originY;
        float m_width;
        float m_height;
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
        CAPTURE(testCase.m_originX);
        CAPTURE(testCase.m_originY);
        CAPTURE(testCase.m_width);
        CAPTURE(testCase.m_height);
        auto const result = geometry(
            testCase.m_originX,
            testCase.m_originY,
            testCase.m_width,
            testCase.m_height
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::InternalInvariant
        );
    }
}

TEST_CASE("client geometry builds a transform with the captured frame extent")
{
    auto const client = geometry(100.0F, 50.0F, 1'600.0F, 900.0F);
    REQUIRE(client.has_value());
    auto const transform = client->transformFor(800, 450);

    REQUIRE(transform.has_value());
    CHECK(transform->frameSize() == std::pair{std::uint32_t{800}, std::uint32_t{450}});
    CHECK(
        transform->desktopToFrame(uf::Point<uf::DesktopSpace>{100.0F, 50.0F})
        == uf::Point<uf::FrameSpace>{0.0F, 0.0F}
    );
    CHECK(
        transform->desktopToFrame(uf::Point<uf::DesktopSpace>{1'700.0F, 950.0F})
        == uf::Point<uf::FrameSpace>{800.0F, 450.0F}
    );
}

TEST_CASE("client geometry integer extent requires whole pixels")
{
    auto const whole = geometry(0.0F, 0.0F, 2'560.0F, 1'440.0F);
    REQUIRE(whole.has_value());
    auto const extent = uf::controller_detail::clientIntegerExtent(*whole);
    REQUIRE(extent.has_value());
    CHECK(*extent == std::pair{std::uint32_t{2'560}, std::uint32_t{1'440}});

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
        auto const result = uf::controller_detail::clientIntegerExtent(*client);
        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::CaptureUnavailable
        );
    }
}

TEST_CASE("capture geometry accepts matching content and surface sizes")
{
    auto state = uf::controller_detail::CaptureGeometryState::create({800, 450});
    REQUIRE(state.has_value());
    auto const confirmed = state->observeContentSize({800, 450});
    REQUIRE(confirmed.has_value());

    CHECK(state->observeSurfaceSize(*confirmed, 800, 450).has_value());
    CHECK(state->ensureActive().has_value());
}

TEST_CASE("content size change invalidates capture geometry until rebuild")
{
    auto state = uf::controller_detail::CaptureGeometryState::create({800, 450});
    REQUIRE(state.has_value());
    auto const changed = state->observeContentSize({801, 450});
    REQUIRE_FALSE(changed.has_value());
    CHECK(automationKind(changed.error()) == uf::AutomationErrorKind::CaptureUnavailable);

    auto const latched = state->observeContentSize({800, 450});
    REQUIRE_FALSE(latched.has_value());
    CHECK(automationKind(latched.error()) == uf::AutomationErrorKind::CaptureUnavailable);
}

TEST_CASE("invalid content size and surface mismatch fail closed")
{
    for (
        auto const size : std::array{
            uf::controller_detail::CaptureSize{0, 450},
            uf::controller_detail::CaptureSize{-1, 450},
        }
    )
    {
        CAPTURE(size.m_width);
        CAPTURE(size.m_height);
        auto state = uf::controller_detail::CaptureGeometryState::create({800, 450});
        REQUIRE(state.has_value());
        auto const result = state->observeContentSize(size);
        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::CaptureUnavailable
        );
        CHECK_FALSE(state->ensureActive().has_value());
    }

    auto state = uf::controller_detail::CaptureGeometryState::create({800, 450});
    REQUIRE(state.has_value());
    auto const mismatch = state->observeSurfaceSize({800, 450}, 800, 451);
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(
        automationKind(mismatch.error())
        == uf::AutomationErrorKind::CaptureUnavailable
    );
    CHECK_FALSE(state->ensureActive().has_value());
}

TEST_CASE("capture options reject zero timeout and default to a positive second")
{
    for (
        auto const timeout : std::array{
            uf::MonotonicInstant::Duration::zero(),
            -uf::MonotonicInstant::Duration{1},
        }
    )
    {
        CAPTURE(timeout.count());
        auto const invalid = uf::WgcCaptureOptions::create(timeout, false);
        REQUIRE_FALSE(invalid.has_value());
        CHECK(
            automationKind(invalid.error())
            == uf::AutomationErrorKind::InternalInvariant
        );
    }

    auto const options = uf::WgcCaptureOptions{};
    CHECK(options.captureStallTimeout() == uf::g_defaultCaptureStallTimeout);
    CHECK_FALSE(options.requireBorderless());
    CHECK(options.captureStallTimeout() > uf::MonotonicInstant::Duration::zero());
}
