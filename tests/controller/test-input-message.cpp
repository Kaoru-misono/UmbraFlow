#include <controller/detail/input-message.hpp>
#include <controller/input.hpp>

#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
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

TEST_CASE("keydown lParam places scan code and clears transition bits")
{
    CHECK(
        uf::controller_detail::keyboardLParamBits(
            0x1EU,
            false,
            uf::controller_detail::KeyTransition::Down
        ) == 0x001E'0001U
    );
}

TEST_CASE("keyup lParam sets previous and transition bits")
{
    CHECK(
        uf::controller_detail::keyboardLParamBits(
            0x1EU,
            false,
            uf::controller_detail::KeyTransition::Up
        ) == 0xC01E'0001U
    );
}

TEST_CASE("extended-key lParam sets bit 24")
{
    CHECK(
        uf::controller_detail::keyboardLParamBits(
            0x4BU,
            true,
            uf::controller_detail::KeyTransition::Down
        ) == 0x014B'0001U
    );
    CHECK(
        uf::controller_detail::keyboardLParamBits(
            0x4BU,
            true,
            uf::controller_detail::KeyTransition::Up
        ) == 0xC14B'0001U
    );
}

TEST_CASE("pointer lParam packs y high and x low")
{
    CHECK(uf::controller_detail::pointerLParamBits(pixel(100, 200)) == 0x00C8'0064U);
    CHECK(uf::controller_detail::pointerLParamBits(pixel(0, 0)) == 0x0000'0000U);
    CHECK(
        uf::controller_detail::pointerLParamBits(
            pixel(
                std::numeric_limits<std::int16_t>::max(),
                std::numeric_limits<std::int16_t>::max()
            )
        ) == 0x7FFF'7FFFU
    );
}

TEST_CASE("client pixels reject negative and wrapping coordinates")
{
    struct InvalidPixel final
    {
        std::int32_t m_x;
        std::int32_t m_y;
    };

    for (auto const testCase : std::array{
        InvalidPixel{-1, 0},
        InvalidPixel{0, -1},
        InvalidPixel{32'768, 0},
        InvalidPixel{0, 32'768},
    })
    {
        auto const result = uf::ClientPixel::create(testCase.m_x, testCase.m_y);
        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == uf::AutomationErrorKind::ActionRejected);
    }
}

TEST_CASE("key specs carry the virtual key in wParam and flags in lParam")
{
    auto const key = uf::KeyInput{0x0041U};
    auto const down = uf::controller_detail::keySpec(
        key,
        0x1EU,
        uf::controller_detail::KeyTransition::Down
    );
    CHECK(down.m_message == uf::controller_detail::wmKeyDown);
    CHECK(down.m_wParam == 0x41U);
    CHECK(down.m_lParam == std::intptr_t{0x001E'0001});

    auto const up = uf::controller_detail::keySpec(
        key,
        0x1EU,
        uf::controller_detail::KeyTransition::Up
    );
    CHECK(up.m_message == uf::controller_detail::wmKeyUp);
    CHECK(up.m_lParam == std::intptr_t{0xC01E'0001});
}

TEST_CASE("numpad Enter preserves its extended bit across down and up")
{
    auto const mainEnter = uf::KeyInput{0x000DU};
    auto const numpadEnter = uf::KeyInput::numpadEnter();
    CHECK_FALSE(mainEnter.isExtended());
    CHECK(numpadEnter.isExtended());
    CHECK(mainEnter.virtualKey() == numpadEnter.virtualKey());

    auto const down = uf::controller_detail::keySpec(
        numpadEnter,
        0x1CU,
        uf::controller_detail::KeyTransition::Down
    );
    auto const up = uf::controller_detail::keySpec(
        numpadEnter,
        0x1CU,
        uf::controller_detail::KeyTransition::Up
    );
    CHECK(down.m_lParam == std::intptr_t{0x011C'0001});
    CHECK(up.m_lParam == std::intptr_t{0xC11C'0001});
}

TEST_CASE("pointer specs set the button mask only while down")
{
    auto const clientPixel = pixel(100, 200);
    CHECK(
        uf::controller_detail::pointerSpec(
            uf::controller_detail::PointerMessage::Move,
            clientPixel
        ) == uf::controller_detail::PostSpec{
            .m_message = uf::controller_detail::wmMouseMove,
            .m_wParam = 0,
            .m_lParam = 0x00C8'0064,
        }
    );
    CHECK(
        uf::controller_detail::pointerSpec(
            uf::controller_detail::PointerMessage::MoveWithLeftButton,
            clientPixel
        ) == uf::controller_detail::PostSpec{
            .m_message = uf::controller_detail::wmMouseMove,
            .m_wParam = uf::controller_detail::leftButtonMask,
            .m_lParam = 0x00C8'0064,
        }
    );
    CHECK(
        uf::controller_detail::pointerSpec(
            uf::controller_detail::PointerMessage::LeftDown,
            clientPixel
        ) == uf::controller_detail::PostSpec{
            .m_message = uf::controller_detail::wmLeftButtonDown,
            .m_wParam = uf::controller_detail::leftButtonMask,
            .m_lParam = 0x00C8'0064,
        }
    );
    CHECK(
        uf::controller_detail::pointerSpec(
            uf::controller_detail::PointerMessage::LeftUp,
            clientPixel
        ) == uf::controller_detail::PostSpec{
            .m_message = uf::controller_detail::wmLeftButtonUp,
            .m_wParam = 0,
            .m_lParam = 0x00C8'0064,
        }
    );
}

TEST_CASE("character and Unicode-character specs post code points directly")
{
    CHECK(
        uf::controller_detail::charSpec(0x0041U) == uf::controller_detail::PostSpec{
            .m_message = uf::controller_detail::wmChar,
            .m_wParam = 0x41U,
            .m_lParam = 0,
        }
    );
    CHECK(
        uf::controller_detail::unicharSpec(U'\U0001F600')
        == uf::controller_detail::PostSpec{
            .m_message = uf::controller_detail::wmUnichar,
            .m_wParam = 0x0001'F600U,
            .m_lParam = 0,
        }
    );
}

TEST_CASE("UTF-8 text follows the WM_CHAR UTF-16 code-unit path")
{
    auto const codeUnits = uf::controller_detail::utf16CodeUnits(
        "A\xF0\x9F\x98\x80"
    );
    REQUIRE(codeUnits.has_value());
    CHECK(
        *codeUnits == std::vector<std::uint16_t>{
            0x0041U,
            0xD83DU,
            0xDE00U,
        }
    );

    auto const invalid = uf::controller_detail::utf16CodeUnits("\xF0\x28\x8C\x28");
    REQUIRE_FALSE(invalid.has_value());
    CHECK(automationKind(invalid.error()) == uf::AutomationErrorKind::ActionRejected);
}

TEST_CASE("extended-key set matches navigation and right modifiers")
{
    for (auto const virtualKey : std::array<std::uint16_t, 16>{
        0x0025U,
        0x0026U,
        0x0027U,
        0x0028U,
        0x002DU,
        0x002EU,
        0x0024U,
        0x0023U,
        0x0021U,
        0x0022U,
        0x00A3U,
        0x00A5U,
        0x0090U,
        0x006FU,
        0x002CU,
        0x0003U,
    })
    {
        CHECK(uf::controller_detail::isExtendedKey(virtualKey));
    }
    for (auto const virtualKey : std::array<std::uint16_t, 3>{0x0041U, 0x0030U, 0x000DU})
    {
        CHECK_FALSE(uf::controller_detail::isExtendedKey(virtualKey));
    }
}

TEST_CASE("delivery to an invalid window audits before reporting disconnect")
{
    auto audit = uf::AuditLog{};
    auto const bogus = uf::WindowHandle{std::intptr_t{0xDEAD'BEEF}};
    auto const result = uf::controller_detail::deliver(
        bogus,
        uf::controller_detail::charSpec(0x0041U),
        audit
    );

    REQUIRE_FALSE(result.has_value());
    CHECK(automationKind(result.error()) == uf::AutomationErrorKind::ControllerDisconnected);
    REQUIRE(audit.size() == 1U);
    CHECK(audit.records()[0].m_message == uf::controller_detail::wmChar);
    CHECK(audit.records()[0].m_wParam == 0x41U);
    CHECK(audit.records()[0].m_target == std::uintptr_t{0xDEAD'BEEF});
}

TEST_CASE("null and broadcast delivery targets fail closed without posting")
{
    for (auto const handle : std::array{
        std::intptr_t{0},
        std::intptr_t{0xFFFF},
    })
    {
        INFO("window handle: ", handle);
        auto audit = uf::AuditLog{};
        auto const result = uf::controller_detail::deliver(
            uf::WindowHandle{handle},
            uf::controller_detail::charSpec(0x0041U),
            audit
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(
            automationKind(result.error())
            == uf::AutomationErrorKind::ControllerDisconnected
        );
        CHECK(audit.empty());
    }
}
