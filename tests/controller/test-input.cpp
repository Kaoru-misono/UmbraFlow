#include <controller/detail/input-held.hpp>
#include <controller/detail/input-revalidation.hpp>
#include <controller/input.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]]
    auto target(
        std::intptr_t window,
        uf::SessionId session = uf::SessionId{1},
        uf::TargetGeneration generation = uf::TargetGeneration{},
        std::uint32_t width = 800,
        std::uint32_t height = 450
    ) -> uf::DeliveryTarget
    {
        auto result = uf::DeliveryTarget::create(
            uf::WindowHandle{window},
            session,
            generation,
            width,
            height
        );
        REQUIRE(result.has_value());
        return *std::move(result);
    }

    [[nodiscard]]
    auto pixel(std::int32_t x, std::int32_t y) -> uf::ClientPixel
    {
        auto const result = uf::ClientPixel::create(x, y);
        REQUIRE(result.has_value());
        return *result;
    }

    [[nodiscard]]
    auto automationKind(uf::Error const& error) -> uf::AutomationErrorKind
    {
        auto const kind = uf::automationErrorKind(error);
        REQUIRE(kind.has_value());
        return *kind;
    }

    [[nodiscard]]
    auto observationLease(
        uf::TargetGeneration generation = uf::TargetGeneration{}
    ) -> uf::ObservationLease
    {
        auto const transform = uf::CoordinateTransform::create(
            uf::Point<uf::DesktopSpace>{0.0F, 0.0F},
            1.0F,
            1.0F,
            1,
            1
        );
        REQUIRE(transform.has_value());
        auto const pixels = std::make_shared<uf::FrameBuffer const>(
            std::vector<std::byte>(4)
        );
        auto const frame = uf::Frame::create(
            uf::FrameId{1},
            uf::SessionId{1},
            generation,
            uf::MonotonicInstant::now(),
            1,
            1,
            4,
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
}

TEST_CASE("delivery targets reject empty client areas as target unavailable")
{
    struct EmptySize final
    {
        std::uint32_t m_width;
        std::uint32_t m_height;
    };
    for (auto const size : std::array{
        EmptySize{0, 10},
        EmptySize{10, 0},
        EmptySize{0, 0},
    })
    {
        auto const result = uf::DeliveryTarget::create(
            uf::WindowHandle{0x10},
            uf::SessionId{1},
            uf::TargetGeneration{},
            size.m_width,
            size.m_height
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == uf::AutomationErrorKind::TargetUnavailable);
    }
}

TEST_CASE("delivery targets expose their fields")
{
    auto const deliveryTarget = target(0x20);
    CHECK(deliveryTarget.windowHandle() == uf::WindowHandle{0x20});
    CHECK(deliveryTarget.sessionId() == uf::SessionId{1});
    CHECK(deliveryTarget.generation() == uf::TargetGeneration{});
    CHECK(deliveryTarget.clientWidth() == 800U);
    CHECK(deliveryTarget.clientHeight() == 450U);
}

TEST_CASE("release held on a dead target still empties and reports")
{
    auto const deliveryTarget = target(std::intptr_t{0xDEAD'BEEF});
    auto held = uf::HeldInputs{};
    auto const keyDown = uf::controller_detail::HeldInputsAccess::onKeyDown(
        held,
        deliveryTarget,
        uf::KeyInput{0x0041U}
    );
    REQUIRE(keyDown.has_value());
    CHECK(*keyDown);
    CHECK(
        uf::controller_detail::HeldInputsAccess::onPointerDown(
            held,
            deliveryTarget,
            uf::PointerButton::Left,
            pixel(1, 2)
        )
    );
    auto audit = uf::AuditLog{};

    auto const outcomes = uf::releaseHeld(deliveryTarget, held, audit);

    REQUIRE(outcomes.size() == 2U);
    CHECK(held.empty());
    CHECK(audit.size() == 2U);
    CHECK(
        std::ranges::all_of(
            outcomes,
            [](uf::ReleaseOutcome const& outcome)
            {
                return (
                    !outcome.m_result.has_value()
                    && automationKind(outcome.m_result.error())
                        == uf::AutomationErrorKind::ControllerDisconnected
                );
            }
        )
    );
}

TEST_CASE("refreshed targets must match window session and generation")
{
    auto const original = target(0x10);
    auto const sameIdentityNewGeometry = target(0x10, uf::SessionId{1}, {}, 640, 360);
    CHECK(
        uf::controller_detail::ensureSameDeliveryIdentity(
            original,
            sameIdentityNewGeometry
        )
    );

    auto const next = original.generation().next();
    REQUIRE(next.has_value());
    for (auto const& changed : std::array{
        target(0x20),
        target(0x10, uf::SessionId{2}),
        target(0x10, uf::SessionId{1}, *next),
    })
    {
        auto const result = uf::controller_detail::ensureSameDeliveryIdentity(
            original,
            changed
        );
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == uf::AutomationErrorKind::StaleObservation);
    }
}

TEST_CASE("release held never posts to a replacement target")
{
    auto const original = target(0x10);
    auto const next = original.generation().next();
    REQUIRE(next.has_value());
    auto const replacement = target(0x20, uf::SessionId{1}, *next);
    auto held = uf::HeldInputs{};
    auto const keyDown = uf::controller_detail::HeldInputsAccess::onKeyDown(
        held,
        original,
        uf::KeyInput::numpadEnter()
    );
    REQUIRE(keyDown.has_value());
    CHECK(*keyDown);
    auto audit = uf::AuditLog{};

    auto const outcomes = uf::releaseHeld(replacement, held, audit);

    REQUIRE(outcomes.size() == 1U);
    CHECK(audit.empty());
    REQUIRE_FALSE(outcomes[0].m_result.has_value());
    CHECK(
        automationKind(outcomes[0].m_result.error())
        == uf::AutomationErrorKind::ActionRejected
    );
    CHECK(held.empty());
}

TEST_CASE("long press rejects a negative hold before delivery")
{
    auto const deliveryTarget = target(0);
    auto held = uf::HeldInputs{};
    auto audit = uf::AuditLog{};
    auto refreshCalled = false;

    auto const result = uf::longPress(
        deliveryTarget,
        observationLease(),
        uf::Point<uf::ClientSpace>{0.0F, 0.0F},
        uf::MonotonicInstant::Duration{-1},
        held,
        audit,
        [&refreshCalled, deliveryTarget]() -> uf::Result<uf::DeliveryTarget>
        {
            refreshCalled = true;
            return deliveryTarget;
        }
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::ActionRejected);
    CHECK(result.error().message().contains("duration"));
    CHECK_FALSE(refreshCalled);
    CHECK(held.empty());
    CHECK(audit.empty());
}

TEST_CASE("long press rejects an empty refresh callback before delivery")
{
    auto const deliveryTarget = target(0);
    auto held = uf::HeldInputs{};
    auto audit = uf::AuditLog{};

    auto const result = uf::longPress(
        deliveryTarget,
        observationLease(),
        uf::Point<uf::ClientSpace>{0.0F, 0.0F},
        uf::MonotonicInstant::Duration::zero(),
        held,
        audit,
        std::move_only_function<uf::Result<uf::DeliveryTarget>()>{}
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(
        automationKind(result.error())
        == uf::AutomationErrorKind::InternalInvariant
    );
    CHECK(held.empty());
    CHECK_FALSE(held.holdsPointer(uf::PointerButton::Left));
    CHECK(audit.empty());
}
