#include <controller/detail/input-held.hpp>
#include <controller/input.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto target(intptr window) -> DeliveryTarget
        {
            auto result = DeliveryTarget::create(
                WindowHandle{window},
                CaptureSessionId{1},
                TargetGeneration{},
                800,
                450
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
    }

    TEST_CASE("key and pointer holds add and clear")
    {
        auto held = HeldInputs{};
        auto const original = target(0x10);
        auto const key = KeyInput{0x0041U};
        CHECK(held.empty());

        auto const firstDown = controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            key
        );
        REQUIRE(firstDown.has_value());
        CHECK(*firstDown);
        auto const repeatedDown = controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            key
        );
        REQUIRE(repeatedDown.has_value());
        CHECK_FALSE(*repeatedDown);
        CHECK(
            controller_detail::HeldInputsAccess::onPointerDown(
                held,
                original,
                PointerButton::Left,
                pixel(5, 6)
            )
        );
        CHECK(held.size() == 2U);
        CHECK(held.holdsKey(key));
        CHECK(held.holdsPointer(PointerButton::Left));

        auto const firstUp = controller_detail::HeldInputsAccess::onKeyUp(
            held,
            original,
            key
        );
        REQUIRE(firstUp.has_value());
        CHECK(*firstUp);
        auto const repeatedUp = controller_detail::HeldInputsAccess::onKeyUp(
            held,
            original,
            key
        );
        REQUIRE(repeatedUp.has_value());
        CHECK_FALSE(*repeatedUp);
        auto const pointerUp = controller_detail::HeldInputsAccess::onPointerUp(
            held,
            original,
            PointerButton::Left
        );
        REQUIRE(pointerUp.has_value());
        CHECK(*pointerUp);
        CHECK(held.empty());
    }

    TEST_CASE("release all empties held state and reports each Up")
    {
        auto held = HeldInputs{};
        auto const original = target(0x10);
        auto const firstKey = KeyInput{0x0041U};
        auto const secondKey = KeyInput{0x0042U};
        REQUIRE(
            controller_detail::HeldInputsAccess::onKeyDown(
                held,
                original,
                firstKey
            )
        );
        REQUIRE(
            controller_detail::HeldInputsAccess::onKeyDown(
                held,
                original,
                secondKey
            )
        );
        REQUIRE(
            controller_detail::HeldInputsAccess::onPointerDown(
                held,
                original,
                PointerButton::Left,
                pixel(9, 9)
            )
        );

        auto posted = std::vector<HeldInput>{};
        auto const outcomes = controller_detail::HeldInputsAccess::releaseAll(
            held,
            original,
            [&posted](HeldInput input) -> Status
            {
                posted.emplace_back(input);
                return ok();
            }
        );

        REQUIRE(outcomes.size() == 3U);
        CHECK(
            std::ranges::all_of(
                outcomes,
                [](ReleaseOutcome const& outcome)
                {
                    return outcome.result.has_value();
                }
            )
        );
        CHECK(held.empty());
        CHECK(std::ranges::find(posted, HeldInput{firstKey}) != posted.end());
        CHECK(std::ranges::find(posted, HeldInput{secondKey}) != posted.end());
        CHECK(
            std::ranges::find(
                posted,
                HeldInput{
                    HeldPointerInput{
                        PointerButton::Left,
                        pixel(9, 9)
                    }
                }
            ) != posted.end()
        );
    }

    TEST_CASE("release all drops held state even when every Up fails")
    {
        auto held = HeldInputs{};
        auto const original = target(0x10);
        REQUIRE(
            controller_detail::HeldInputsAccess::onKeyDown(
                held,
                original,
                KeyInput{0x0041U}
            )
        );
        REQUIRE(
            controller_detail::HeldInputsAccess::onPointerDown(
                held,
                original,
                PointerButton::Left,
                pixel(1, 2)
            )
        );

        auto const outcomes = controller_detail::HeldInputsAccess::releaseAll(
            held,
            original,
            [](HeldInput) -> Status
            {
                return fail(
                    AutomationErrorKind::ControllerDisconnected,
                    "window gone"
                );
            }
        );

        REQUIRE(outcomes.size() == 2U);
        CHECK(
            std::ranges::all_of(
                outcomes,
                [](ReleaseOutcome const& outcome)
                {
                    return !outcome.result.has_value();
                }
            )
        );
        CHECK(held.empty());
    }

    TEST_CASE("mismatched targets reject actions and cleanup never posts")
    {
        auto const original = target(0x10);
        auto const replacement = target(0x20);
        auto held = HeldInputs{};
        REQUIRE(
            controller_detail::HeldInputsAccess::onKeyDown(
                held,
                original,
                KeyInput::numpadEnter()
            )
        );

        auto const mismatch = controller_detail::HeldInputsAccess::ensureTarget(
            held,
            replacement
        );
        REQUIRE_FALSE(mismatch.has_value());
        CHECK(automationKind(mismatch.error()) == AutomationErrorKind::ActionRejected);

        auto postCount = 0;
        auto const outcomes = controller_detail::HeldInputsAccess::releaseAll(
            held,
            replacement,
            [&postCount](HeldInput) -> Status
            {
                ++postCount;
                return ok();
            }
        );
        CHECK(postCount == 0);
        REQUIRE(outcomes.size() == 1U);
        REQUIRE_FALSE(outcomes[0].result.has_value());
        CHECK(
            automationKind(outcomes[0].result.error())
            == AutomationErrorKind::ActionRejected
        );
        CHECK(held.empty());
    }
}
