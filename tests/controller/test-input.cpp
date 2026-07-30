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
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto target(
            intptr window,
            CaptureSessionId session = CaptureSessionId{1},
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
                CaptureSessionId{1},
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

    TEST_CASE("supported key names map one-to-one onto distinct virtual keys")
    {
        auto names = std::vector<std::string>{};
        for (auto letter = 'A'; letter <= 'Z'; ++letter)
        {
            names.emplace_back(1U, letter);
        }
        for (auto digit = '0'; digit <= '9'; ++digit)
        {
            names.emplace_back(1U, digit);
        }
        for (auto number = uint32{1}; number <= 12U; ++number)
        {
            names.emplace_back(std::format("F{}", number));
        }
        REQUIRE(names.size() == 48U);

        auto virtualKeys = std::vector<uint16>{};
        for (auto const& name : names)
        {
            CAPTURE(name);
            auto const key = KeyInput::fromName(name);
            REQUIRE(key.has_value());
            CHECK_FALSE(key->isExtended());
            virtualKeys.emplace_back(key->virtualKey());
        }
        std::ranges::sort(virtualKeys);
        CHECK(std::ranges::adjacent_find(virtualKeys) == virtualKeys.end());

        // Anchored to the Win32 virtual-key values the target actually reads, so
        // a shifted table cannot pass on internal consistency alone.
        for (auto const& [name, virtualKey] : std::array{
            std::pair{std::string_view{"A"}, uint16{0x0041U}},
            std::pair{std::string_view{"E"}, uint16{0x0045U}},
            std::pair{std::string_view{"Z"}, uint16{0x005AU}},
            std::pair{std::string_view{"0"}, uint16{0x0030U}},
            std::pair{std::string_view{"8"}, uint16{0x0038U}},
            // "F" is the letter key, not a truncated function key.
            std::pair{std::string_view{"F"}, uint16{0x0046U}},
            std::pair{std::string_view{"F1"}, uint16{0x0070U}},
            std::pair{std::string_view{"F3"}, uint16{0x0072U}},
            std::pair{std::string_view{"F12"}, uint16{0x007BU}},
        })
        {
            CAPTURE(name);
            auto const key = KeyInput::fromName(name);
            REQUIRE(key.has_value());
            CHECK(key->virtualKey() == virtualKey);
        }
    }

    TEST_CASE("unsupported key names are rejected instead of guessed")
    {
        for (auto const name : std::array<std::string_view, 11>{
            "",
            "e",
            "f1",
            "F0",
            "F01",
            "F13",
            "F1 ",
            "AB",
            "10",
            "ENTER",
            "+",
        })
        {
            CAPTURE(name);
            auto const result = KeyInput::fromName(name);
            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationKind(result.error())
                == AutomationErrorKind::ActionRejected
            );
        }
    }

    TEST_CASE("delivery targets reject empty client areas as target unavailable")
    {
        struct EmptySize final
        {
            uint32 width{};
            uint32 height{};
        };
        for (auto const size : std::array{
            EmptySize{0, 10},
            EmptySize{10, 0},
            EmptySize{0, 0},
        })
        {
            auto const result = DeliveryTarget::create(
                WindowHandle{0x10},
                CaptureSessionId{1},
                TargetGeneration{},
                size.width,
                size.height
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
                        !outcome.result.has_value()
                        && automationKind(outcome.result.error())
                            == AutomationErrorKind::ControllerDisconnected
                    );
                }
            )
        );
    }

    TEST_CASE("refreshed targets must match window session and generation")
    {
        auto const original = target(0x10);
        auto const sameIdentityNewGeometry = target(0x10, CaptureSessionId{1}, {}, 640, 360);
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
            target(0x10, CaptureSessionId{2}),
            target(0x10, CaptureSessionId{1}, *next),
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
        auto const replacement = target(0x20, CaptureSessionId{1}, *next);
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
        REQUIRE_FALSE(outcomes[0].result.has_value());
        CHECK(
            automationKind(outcomes[0].result.error())
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
