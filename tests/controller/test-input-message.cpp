#include <controller/detail/input-message.hpp>
#include <controller/input.hpp>

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <vector>

namespace uf
{
    namespace
    {
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

    TEST_CASE("keydown lParam places scan code and clears transition bits")
    {
        CHECK(
            controller_detail::keyboardLParamBits(
                0x1EU,
                false,
                controller_detail::KeyTransition::Down
            ) == 0x001E'0001U
        );
    }

    TEST_CASE("keyup lParam sets previous and transition bits")
    {
        CHECK(
            controller_detail::keyboardLParamBits(
                0x1EU,
                false,
                controller_detail::KeyTransition::Up
            ) == 0xC01E'0001U
        );
    }

    TEST_CASE("extended-key lParam sets bit 24")
    {
        CHECK(
            controller_detail::keyboardLParamBits(
                0x4BU,
                true,
                controller_detail::KeyTransition::Down
            ) == 0x014B'0001U
        );
        CHECK(
            controller_detail::keyboardLParamBits(
                0x4BU,
                true,
                controller_detail::KeyTransition::Up
            ) == 0xC14B'0001U
        );
    }

    TEST_CASE("pointer lParam packs y high and x low")
    {
        CHECK(controller_detail::pointerLParamBits(pixel(100, 200)) == 0x00C8'0064U);
        CHECK(controller_detail::pointerLParamBits(pixel(0, 0)) == 0x0000'0000U);
        CHECK(
            controller_detail::pointerLParamBits(
                pixel(
                    std::numeric_limits<int16>::max(),
                    std::numeric_limits<int16>::max()
                )
            ) == 0x7FFF'7FFFU
        );
    }

    TEST_CASE("client pixels reject negative and wrapping coordinates")
    {
        struct InvalidPixel final
        {
            int32 m_x;
            int32 m_y;
        };

        for (auto const testCase : std::array{
            InvalidPixel{-1, 0},
            InvalidPixel{0, -1},
            InvalidPixel{32'768, 0},
            InvalidPixel{0, 32'768},
        })
        {
            auto const result = ClientPixel::create(testCase.m_x, testCase.m_y);
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::ActionRejected);
        }
    }

    TEST_CASE("key specs carry the virtual key in wParam and flags in lParam")
    {
        auto const key = KeyInput{0x0041U};
        auto const down = controller_detail::keySpec(
            key,
            0x1EU,
            controller_detail::KeyTransition::Down
        );
        CHECK(down.m_message == controller_detail::g_wmKeyDown);
        CHECK(down.m_wParam == 0x41U);
        CHECK(down.m_lParam == intptr{0x001E'0001});

        auto const up = controller_detail::keySpec(
            key,
            0x1EU,
            controller_detail::KeyTransition::Up
        );
        CHECK(up.m_message == controller_detail::g_wmKeyUp);
        CHECK(up.m_lParam == intptr{0xC01E'0001});
    }

    TEST_CASE("numpad Enter preserves its extended bit across down and up")
    {
        auto const mainEnter = KeyInput{0x000DU};
        auto const numpadEnter = KeyInput::numpadEnter();
        CHECK_FALSE(mainEnter.isExtended());
        CHECK(numpadEnter.isExtended());
        CHECK(mainEnter.virtualKey() == numpadEnter.virtualKey());

        auto const down = controller_detail::keySpec(
            numpadEnter,
            0x1CU,
            controller_detail::KeyTransition::Down
        );
        auto const up = controller_detail::keySpec(
            numpadEnter,
            0x1CU,
            controller_detail::KeyTransition::Up
        );
        CHECK(down.m_lParam == intptr{0x011C'0001});
        CHECK(up.m_lParam == intptr{0xC11C'0001});
    }

    TEST_CASE("pointer specs set the button mask only while down")
    {
        auto const clientPixel = pixel(100, 200);
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::Move,
                clientPixel
            ) == controller_detail::PostSpec{
                .m_message = controller_detail::g_wmMouseMove,
                .m_wParam  = 0,
                .m_lParam = 0x00C8'0064,
            }
        );
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::MoveWithLeftButton,
                clientPixel
            ) == controller_detail::PostSpec{
                .m_message = controller_detail::g_wmMouseMove,
                .m_wParam  = controller_detail::g_leftButtonMask,
                .m_lParam = 0x00C8'0064,
            }
        );
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::LeftDown,
                clientPixel
            ) == controller_detail::PostSpec{
                .m_message = controller_detail::g_wmLeftButtonDown,
                .m_wParam  = controller_detail::g_leftButtonMask,
                .m_lParam = 0x00C8'0064,
            }
        );
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::LeftUp,
                clientPixel
            ) == controller_detail::PostSpec{
                .m_message = controller_detail::g_wmLeftButtonUp,
                .m_wParam  = 0,
                .m_lParam = 0x00C8'0064,
            }
        );
    }

    TEST_CASE("character and Unicode-character specs post code points directly")
    {
        CHECK(
            controller_detail::charSpec(0x0041U) == controller_detail::PostSpec{
                .m_message = controller_detail::g_wmChar,
                .m_wParam  = 0x41U,
                .m_lParam  = 1,
            }
        );
        CHECK(
            controller_detail::unicharSpec(U'\U0001F600')
            == controller_detail::PostSpec{
                .m_message = controller_detail::g_wmUnichar,
                .m_wParam = 0x0001'F600U,
                .m_lParam = 1,
            }
        );
    }

    TEST_CASE("UTF-8 text follows the WM_CHAR UTF-16 code-unit path")
    {
        auto const codeUnits = controller_detail::utf16CodeUnits(
            "A\xF0\x9F\x98\x80"
        );
        REQUIRE(codeUnits.has_value());
        CHECK(
            *codeUnits == std::vector<uint16>{
                0x0041U,
                0xD83DU,
                0xDE00U,
            }
        );

        auto const invalid = controller_detail::utf16CodeUnits("\xF0\x28\x8C\x28");
        REQUIRE_FALSE(invalid.has_value());
        CHECK(automationKind(invalid.error()) == AutomationErrorKind::ActionRejected);
    }

    TEST_CASE("extended-key set matches navigation and right modifiers")
    {
        for (auto const virtualKey : std::array<uint16, 16>{
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
            CHECK(controller_detail::isExtendedKey(virtualKey));
        }
        for (auto const virtualKey : std::array<uint16, 3>{0x0041U, 0x0030U, 0x000DU})
        {
            CHECK_FALSE(controller_detail::isExtendedKey(virtualKey));
        }
    }

    TEST_CASE("delivery to an invalid window audits before reporting disconnect")
    {
        auto audit = AuditLog{};
        auto const bogus = WindowHandle{intptr{0xDEAD'BEEF}};
        auto const result = controller_detail::deliver(
            bogus,
            controller_detail::charSpec(0x0041U),
            audit
        );

        REQUIRE_FALSE(result.has_value());
        CHECK(automationKind(result.error()) == AutomationErrorKind::ControllerDisconnected);
        REQUIRE(audit.size() == 1U);
        CHECK(audit.records()[0].m_message == controller_detail::g_wmChar);
        CHECK(audit.records()[0].m_wParam == 0x41U);
        CHECK(audit.records()[0].m_target == uintptr{0xDEAD'BEEF});
    }

    TEST_CASE("null and broadcast delivery targets fail closed without posting")
    {
        for (auto const handle : std::array{
            intptr{0},
            intptr{0xFFFF},
        })
        {
            INFO("window handle: ", handle);
            auto audit = AuditLog{};
            auto const result = controller_detail::deliver(
                WindowHandle{handle},
                controller_detail::charSpec(0x0041U),
                audit
            );

            REQUIRE_FALSE(result.has_value());
            CHECK(
                automationKind(result.error())
                == AutomationErrorKind::ControllerDisconnected
            );
            CHECK(audit.empty());
        }
    }
}
