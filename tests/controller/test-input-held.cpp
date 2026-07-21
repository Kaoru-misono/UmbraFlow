#include <controller/detail/input-held.hpp>
#include <controller/input.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]]
    auto target(std::intptr_t window) -> uf::DeliveryTarget
    {
        auto result = uf::DeliveryTarget::create(
            uf::WindowHandle{window},
            uf::SessionId{1},
            uf::TargetGeneration{},
            800,
            450
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
        if (!kind.has_value())
        {
            FAIL("The error did not contain an automation error kind");
            return uf::AutomationErrorKind::InternalInvariant;
        }
        return *kind;
    }
}

TEST_CASE("key and pointer holds add and clear")
{
    auto held = uf::HeldInputs{};
    auto const original = target(0x10);
    auto const key = uf::KeyInput{0x0041U};
    CHECK(held.empty());

    auto const firstDown = uf::controller_detail::HeldInputsAccess::onKeyDown(
        held,
        original,
        key
    );
    REQUIRE(firstDown.has_value());
    CHECK(*firstDown);
    auto const repeatedDown = uf::controller_detail::HeldInputsAccess::onKeyDown(
        held,
        original,
        key
    );
    REQUIRE(repeatedDown.has_value());
    CHECK_FALSE(*repeatedDown);
    CHECK(
        uf::controller_detail::HeldInputsAccess::onPointerDown(
            held,
            original,
            uf::PointerButton::Left,
            pixel(5, 6)
        )
    );
    CHECK(held.size() == 2U);
    CHECK(held.holdsKey(key));
    CHECK(held.holdsPointer(uf::PointerButton::Left));

    auto const firstUp = uf::controller_detail::HeldInputsAccess::onKeyUp(
        held,
        original,
        key
    );
    REQUIRE(firstUp.has_value());
    CHECK(*firstUp);
    auto const repeatedUp = uf::controller_detail::HeldInputsAccess::onKeyUp(
        held,
        original,
        key
    );
    REQUIRE(repeatedUp.has_value());
    CHECK_FALSE(*repeatedUp);
    auto const pointerUp = uf::controller_detail::HeldInputsAccess::onPointerUp(
        held,
        original,
        uf::PointerButton::Left
    );
    REQUIRE(pointerUp.has_value());
    CHECK(*pointerUp);
    CHECK(held.empty());
}

TEST_CASE("release all empties held state and reports each Up")
{
    auto held = uf::HeldInputs{};
    auto const original = target(0x10);
    auto const firstKey = uf::KeyInput{0x0041U};
    auto const secondKey = uf::KeyInput{0x0042U};
    REQUIRE(
        uf::controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            firstKey
        )
    );
    REQUIRE(
        uf::controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            secondKey
        )
    );
    REQUIRE(
        uf::controller_detail::HeldInputsAccess::onPointerDown(
            held,
            original,
            uf::PointerButton::Left,
            pixel(9, 9)
        )
    );

    auto posted = std::vector<uf::HeldInput>{};
    auto const outcomes = uf::controller_detail::HeldInputsAccess::releaseAll(
        held,
        original,
        [&posted](uf::HeldInput input) -> uf::Status
        {
            posted.emplace_back(input);
            return uf::ok();
        }
    );

    REQUIRE(outcomes.size() == 3U);
    CHECK(
        std::ranges::all_of(
            outcomes,
            [](uf::ReleaseOutcome const& outcome)
            {
                return outcome.m_result.has_value();
            }
        )
    );
    CHECK(held.empty());
    CHECK(std::ranges::find(posted, uf::HeldInput{firstKey}) != posted.end());
    CHECK(std::ranges::find(posted, uf::HeldInput{secondKey}) != posted.end());
    CHECK(
        std::ranges::find(
            posted,
            uf::HeldInput{
                uf::HeldPointerInput{
                    uf::PointerButton::Left,
                    pixel(9, 9)
                }
            }
        ) != posted.end()
    );
}

TEST_CASE("release all drops held state even when every Up fails")
{
    auto held = uf::HeldInputs{};
    auto const original = target(0x10);
    REQUIRE(
        uf::controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            uf::KeyInput{0x0041U}
        )
    );
    REQUIRE(
        uf::controller_detail::HeldInputsAccess::onPointerDown(
            held,
            original,
            uf::PointerButton::Left,
            pixel(1, 2)
        )
    );

    auto const outcomes = uf::controller_detail::HeldInputsAccess::releaseAll(
        held,
        original,
        [](uf::HeldInput) -> uf::Status
        {
            return uf::fail(
                uf::AutomationErrorKind::ControllerDisconnected,
                "window gone"
            );
        }
    );

    REQUIRE(outcomes.size() == 2U);
    CHECK(
        std::ranges::all_of(
            outcomes,
            [](uf::ReleaseOutcome const& outcome)
            {
                return !outcome.m_result.has_value();
            }
        )
    );
    CHECK(held.empty());
}

TEST_CASE("mismatched targets reject actions and cleanup never posts")
{
    auto const original = target(0x10);
    auto const replacement = target(0x20);
    auto held = uf::HeldInputs{};
    REQUIRE(
        uf::controller_detail::HeldInputsAccess::onKeyDown(
            held,
            original,
            uf::KeyInput::numpadEnter()
        )
    );

    auto const mismatch = uf::controller_detail::HeldInputsAccess::ensureTarget(
        held,
        replacement
    );
    REQUIRE_FALSE(mismatch.has_value());
    CHECK(automationKind(mismatch.error()) == uf::AutomationErrorKind::ActionRejected);

    auto postCount = 0;
    auto const outcomes = uf::controller_detail::HeldInputsAccess::releaseAll(
        held,
        replacement,
        [&postCount](uf::HeldInput) -> uf::Status
        {
            ++postCount;
            return uf::ok();
        }
    );
    CHECK(postCount == 0);
    REQUIRE(outcomes.size() == 1U);
    REQUIRE_FALSE(outcomes[0].m_result.has_value());
    CHECK(
        automationKind(outcomes[0].m_result.error())
        == uf::AutomationErrorKind::ActionRejected
    );
    CHECK(held.empty());
}
