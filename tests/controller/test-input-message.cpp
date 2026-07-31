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
            int32 x{};
            int32 y{};
        };

        for (auto const testCase : std::array{
            InvalidPixel{-1, 0},
            InvalidPixel{0, -1},
            InvalidPixel{32'768, 0},
            InvalidPixel{0, 32'768},
        })
        {
            auto const result = ClientPixel::create(testCase.x, testCase.y);
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
        CHECK(down.message == controller_detail::k_wmKeyDown);
        CHECK(down.wParam == 0x41U);
        CHECK(down.lParam == intptr{0x001E'0001});

        auto const up = controller_detail::keySpec(
            key,
            0x1EU,
            controller_detail::KeyTransition::Up
        );
        CHECK(up.message == controller_detail::k_wmKeyUp);
        CHECK(up.lParam == intptr{0xC01E'0001});
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
        CHECK(down.lParam == intptr{0x011C'0001});
        CHECK(up.lParam == intptr{0xC11C'0001});
    }

    TEST_CASE("pointer specs set the button mask only while down")
    {
        auto const clientPixel = pixel(100, 200);
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::Move,
                clientPixel
            ) == controller_detail::PostSpec{
                .message = controller_detail::k_wmMouseMove,
                .wParam  = 0,
                .lParam = 0x00C8'0064,
            }
        );
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::MoveWithLeftButton,
                clientPixel
            ) == controller_detail::PostSpec{
                .message = controller_detail::k_wmMouseMove,
                .wParam  = controller_detail::k_leftButtonMask,
                .lParam = 0x00C8'0064,
            }
        );
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::LeftDown,
                clientPixel
            ) == controller_detail::PostSpec{
                .message = controller_detail::k_wmLeftButtonDown,
                .wParam  = controller_detail::k_leftButtonMask,
                .lParam = 0x00C8'0064,
            }
        );
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::LeftUp,
                clientPixel
            ) == controller_detail::PostSpec{
                .message = controller_detail::k_wmLeftButtonUp,
                .wParam  = 0,
                .lParam = 0x00C8'0064,
            }
        );
    }

    TEST_CASE("wheel notches convert to raw deltas and refuse undeliverable counts")
    {
        auto const up = WheelDelta::create(1);
        REQUIRE(up.has_value());
        CHECK(up->notches() == 1);
        CHECK(up->rawUnits() == int16{120});

        auto const down = WheelDelta::create(-3);
        REQUIRE(down.has_value());
        CHECK(down->rawUnits() == int16{-360});

        auto const boundary = WheelDelta::create(k_maxWheelNotches);
        REQUIRE(boundary.has_value());
        CHECK(boundary->rawUnits() == int16{32'760});

        for (auto const notches : std::array<int32, 3>{
            0,
            k_maxWheelNotches + 1,
            -k_maxWheelNotches - 1,
        })
        {
            CAPTURE(notches);
            auto const result = WheelDelta::create(notches);
            REQUIRE_FALSE(result.has_value());
            CHECK(automationKind(result.error()) == AutomationErrorKind::ActionRejected);
        }
    }

    TEST_CASE("screen pixels admit the negative coordinates client pixels refuse")
    {
        auto const offScreen = controller_detail::ScreenPixel::create(-1, -2);
        REQUIRE(offScreen.has_value());
        CHECK(offScreen->x() == -1);
        CHECK(offScreen->y() == -2);
        CHECK_FALSE(ClientPixel::create(-1, -2).has_value());

        // A window on a monitor left of and above the primary one puts its whole
        // client area at negative screen coordinates.
        auto const translated = controller_detail::screenPixelFor(
            controller_detail::ClientOrigin{.x = -1'920, .y = -50},
            pixel(100, 200)
        );
        REQUIRE(translated.has_value());
        CHECK(translated->x() == -1'820);
        CHECK(translated->y() == 150);

        auto const unencodable = controller_detail::screenPixelFor(
            controller_detail::ClientOrigin{.x = 32'700, .y = 0},
            pixel(100, 0)
        );
        REQUIRE_FALSE(unencodable.has_value());
        CHECK(automationKind(unencodable.error()) == AutomationErrorKind::ActionRejected);
    }

    TEST_CASE("wheel lParam is screen space where the button messages are client space")
    {
        auto const clientPixel = pixel(100, 200);
        auto const screenPixel = controller_detail::screenPixelFor(
            controller_detail::ClientOrigin{.x = 30, .y = 40},
            clientPixel
        );
        REQUIRE(screenPixel.has_value());
        auto const delta = WheelDelta::create(1);
        REQUIRE(delta.has_value());

        auto const wheel = controller_detail::wheelSpec(*screenPixel, *delta, false);
        CHECK(wheel.message == controller_detail::k_wmMouseWheel);
        // (130, 240) on screen, not the (100, 200) the same point has in the
        // client area a WM_LBUTTONDOWN would be posted with.
        CHECK(wheel.lParam == intptr{0x00F0'0082});
        CHECK(
            controller_detail::pointerSpec(
                controller_detail::PointerMessage::LeftDown,
                clientPixel
            ).lParam == intptr{0x00C8'0064}
        );
    }

    TEST_CASE("wheel wParam packs the signed delta above the held-button state")
    {
        auto const at = controller_detail::ScreenPixel::create(0, 0);
        auto const forward = WheelDelta::create(2);
        auto const back = WheelDelta::create(-2);
        REQUIRE(at.has_value());
        REQUIRE(forward.has_value());
        REQUIRE(back.has_value());

        CHECK(controller_detail::wheelSpec(*at, *forward, false).wParam == uintptr{0x00F0'0000});
        CHECK(controller_detail::wheelSpec(*at, *back, false).wParam == uintptr{0xFF10'0000});
        CHECK(
            controller_detail::wheelSpec(*at, *forward, true).wParam
            == (uintptr{0x00F0'0000} | controller_detail::k_leftButtonMask)
        );
    }

    TEST_CASE("character and Unicode-character specs post code points directly")
    {
        CHECK(
            controller_detail::charSpec(0x0041U) == controller_detail::PostSpec{
                .message = controller_detail::k_wmChar,
                .wParam  = 0x41U,
                .lParam  = 1,
            }
        );
        CHECK(
            controller_detail::unicharSpec(U'\U0001F600')
            == controller_detail::PostSpec{
                .message = controller_detail::k_wmUnichar,
                .wParam = 0x0001'F600U,
                .lParam = 1,
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
        CHECK(audit.records()[0].message == controller_detail::k_wmChar);
        CHECK(audit.records()[0].wParam == 0x41U);
        CHECK(audit.records()[0].target == uintptr{0xDEAD'BEEF});
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
