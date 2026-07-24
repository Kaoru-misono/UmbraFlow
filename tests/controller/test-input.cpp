#include <controller/detail/input-held.hpp>
#include <controller/detail/input-revalidation.hpp>
#include <controller/input.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto target(
            intptr window,
            SessionId session = SessionId{1},
            TargetGeneration generation = TargetGeneration{},
            uint32 width = 800,
            uint32 height = 450
        ) -> DeliveryTarget
        {
            auto result = DeliveryTarget::create(
                WindowHandle{window},
                session,
                generation,
                width,
                height
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        [[nodiscard]]
        auto pixel(int32 x, int32 y) -> ClientPixel
        {
            auto const result = ClientPixel::create(x, y);
            REQUIRE(result.has_value());
            return *result;
        }

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
        auto observationLease(
            TargetGeneration generation = TargetGeneration{}
        ) -> ObservationLease
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                1.0F,
                1.0F,
                1,
                1
            );
            REQUIRE(transform.has_value());
            auto const pixels = std::make_shared<FrameBuffer const>(
                std::vector<std::byte>(4)
            );
            auto const frame = Frame::create(
                FrameId{1},
                SessionId{1},
                generation,
                MonotonicInstant::now(),
                1,
                1,
                4,
                PixelFormat::Bgra8,
                pixels,
                *transform
            );
            REQUIRE(frame.has_value());
            auto const lease = ObservationLease::forFrame(
                *frame,
                k_defaultMaxActionFrameAge
            );
            REQUIRE(lease.has_value());
            return *lease;
        }
    }

    TEST_CASE("delivery targets reject empty client areas as target unavailable")
    {
        struct EmptySize final
        {
            uint32 m_width{};
            uint32 m_height{};
        };
        for (auto const size : std::array{
            EmptySize{0, 10},
            EmptySize{10, 0},
            EmptySize{0, 0},
        })
        {
            auto const result = DeliveryTarget::create(
                WindowHandle{0x10},
                SessionId{1},
                TargetGeneration{},
                size.m_width,
                size.m_height
            );
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::TargetUnavailable);
        }
    }

    TEST_CASE("release held on a dead target still empties and reports")
    {
        auto const deliveryTarget = target(intptr{0xDEAD'BEEF});
        auto held = HeldInputs{};
        auto const keyDown = controller_detail::HeldInputsAccess::onKeyDown(
            held,
            deliveryTarget,
            KeyInput{0x0041U}
        );
        REQUIRE(keyDown.has_value());
        CHECK(*keyDown);
        CHECK(
            controller_detail::HeldInputsAccess::onPointerDown(
                held,
                deliveryTarget,
                PointerButton::Left,
                pixel(1, 2)
            )
        );
        auto audit = AuditLog{};

        auto const outcomes = releaseHeld(deliveryTarget, held, audit);

        REQUIRE(outcomes.size() == 2U);
        CHECK(held.empty());
        CHECK(audit.size() == 2U);
        CHECK(
            std::ranges::all_of(
                outcomes,
                [](ReleaseOutcome const& outcome)
                {
                    return (
                        !outcome.m_result.has_value()
                        && automationKind(outcome.m_result.error())
                            == AutomationErrorKind::ControllerDisconnected
                    );
                }
            )
        );
    }

    TEST_CASE("refreshed targets must match window session and generation")
    {
        auto const original = target(0x10);
        auto const sameIdentityNewGeometry = target(0x10, SessionId{1}, {}, 640, 360);
        CHECK(
            controller_detail::ensureSameDeliveryIdentity(
                original,
                sameIdentityNewGeometry
            )
        );

        auto const next = original.generation().next();
        REQUIRE(next.has_value());
        for (auto const& changed : std::array{
            target(0x20),
            target(0x10, SessionId{2}),
            target(0x10, SessionId{1}, *next),
        })
        {
            auto const result = controller_detail::ensureSameDeliveryIdentity(
                original,
                changed
            );
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::StaleObservation);
        }
    }

    TEST_CASE("release held never posts to a replacement target")
    {
        auto const original = target(0x10);
        auto const next = original.generation().next();
        REQUIRE(next.has_value());
        auto const replacement = target(0x20, SessionId{1}, *next);
        auto held = HeldInputs{};
        auto const keyDown = controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            KeyInput::numpadEnter()
        );
        REQUIRE(keyDown.has_value());
        CHECK(*keyDown);
        auto audit = AuditLog{};

        auto const outcomes = releaseHeld(replacement, held, audit);

        REQUIRE(outcomes.size() == 1U);
        CHECK(audit.empty());
        REQUIRE_FALSE(outcomes[0].m_result.has_value());
        CHECK(
            automationKind(outcomes[0].m_result.error())
            == AutomationErrorKind::ActionRejected
        );
        CHECK(held.empty());
    }

    TEST_CASE("long press rejects a negative hold before delivery")
    {
        auto const deliveryTarget = target(0);
        auto held          = HeldInputs{};
        auto audit         = AuditLog{};
        auto refreshCalled = false;

        auto const result = longPress(
            deliveryTarget,
            observationLease(),
            Point<ClientSpace>{0.0F, 0.0F},
            MonotonicInstant::Duration{-1},
            held,
            audit,
            [&refreshCalled, deliveryTarget]() -> Result<DeliveryTarget>
            {
                refreshCalled = true;
                return deliveryTarget;
            }
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::ActionRejected);
        CHECK(result.error().message().contains("duration"));
        CHECK_FALSE(refreshCalled);
        CHECK(held.empty());
        CHECK(audit.empty());
    }

    TEST_CASE("long press rejects an empty refresh callback before delivery")
    {
        auto const deliveryTarget = target(0);
        auto held  = HeldInputs{};
        auto audit = AuditLog{};

        auto const result = longPress(
            deliveryTarget,
            observationLease(),
            Point<ClientSpace>{0.0F, 0.0F},
            MonotonicInstant::Duration::zero(),
            held,
            audit,
            std::move_only_function<Result<DeliveryTarget>()>{}
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == AutomationErrorKind::InternalInvariant
        );
        CHECK(held.empty());
        CHECK_FALSE(held.holdsPointer(PointerButton::Left));
        CHECK(audit.empty());
    }
}
