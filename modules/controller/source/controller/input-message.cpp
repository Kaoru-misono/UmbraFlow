#include "detail/input-message.hpp"

#include "platform/windows-controller.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>
#include <domain/key.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <limits>
#include <optional>
#include <string_view>

namespace uf::controller_detail
{
    namespace
    {
        [[nodiscard]]
        constexpr auto lParamFromBits(uint32 bits) noexcept -> intptr
        {
            static_assert(sizeof(intptr) >= sizeof(uint32));
            if constexpr (sizeof(intptr) == sizeof(uint32))
            {
                return std::bit_cast<int32>(bits);
            }
            else
            {
                return static_cast<intptr>(bits);
            }
        }

        // Every mouse message packs its point the same way, y in the high word
        // and x in the low one. Which space those coordinates are in is the
        // caller's contract, not this packing's: the button messages pass client
        // coordinates and WM_MOUSEWHEEL passes screen coordinates. Both pixel
        // types bound their coordinates to a signed 16-bit value, so the
        // conversion below reproduces exactly the word GET_X_LPARAM reads back.
        [[nodiscard]]
        constexpr auto packPointBits(int32 x, int32 y) noexcept -> uint32
        {
            auto const low  = static_cast<uint32>(static_cast<uint16>(x));
            auto const high = static_cast<uint32>(static_cast<uint16>(y));
            return (high << 16U) | low;
        }
    }
}

namespace uf
{
    namespace
    {
        // VK_F1..VK_F12 are consecutive, so the number domain::functionKeyNumber
        // returns indexes them. Which names that family holds is domain's to
        // decide; only the code this base names is Windows knowledge.
        constexpr auto k_virtualKeyF1 = uint16{0x0070U};

        struct NamedKeyCode final
        {
            std::string_view name{};
            uint16           virtualKey{};
        };

        // The virtual key each name in domain::k_namedKeys resolves to. A table
        // rather than an arithmetic rule, because these keys share no consecutive
        // range the way VK_F1..VK_F12 do; a table rather than a comparison chain,
        // because the set is closed and a chain would accept a new member without
        // a branch and say nothing about it.
        constexpr auto k_namedKeyCodes = std::array{
            NamedKeyCode{.name = "ENTER", .virtualKey = 0x000DU},
            NamedKeyCode{.name = "ESC", .virtualKey = 0x001BU},
            NamedKeyCode{.name = "CAPS", .virtualKey = 0x0014U},
            NamedKeyCode{.name = "SHIFT", .virtualKey = 0x0010U},
        };

        [[nodiscard]]
        constexpr auto everyNamedKeyIsMapped() noexcept -> bool
        {
            if (k_namedKeyCodes.size() != k_namedKeys.size())
            {
                return false;
            }
            return std::ranges::all_of(
                k_namedKeys,
                [](std::string_view name)
                {
                    return std::ranges::contains(
                        k_namedKeyCodes,
                        name,
                        &NamedKeyCode::name
                    );
                }
            );
        }

        // fromKeyName is total, and this is what keeps it so. A name the domain
        // set admits with no entry above would fall through to the
        // single-character branch and trip its UF_CHECK at run time on a
        // keystroke the author was entitled to write.
        static_assert(
            everyNamedKeyIsMapped(),
            "every key name domain::KeyName admits must be mapped here"
        );

        [[nodiscard]]
        constexpr auto namedKeyVirtualKey(
            std::string_view name
        ) noexcept -> std::optional<uint16>
        {
            auto const found = std::ranges::find(
                k_namedKeyCodes,
                name,
                &NamedKeyCode::name
            );
            if (found == k_namedKeyCodes.end())
            {
                return std::nullopt;
            }
            return found->virtualKey;
        }
    }

    auto ClientPixel::create(int32 x, int32 y) -> Result<ClientPixel>
    {
        if (
            x < std::numeric_limits<int16>::min()
            || x > std::numeric_limits<int16>::max()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client x coordinate {} is outside the encodable range 0..={}",
                    x,
                    std::numeric_limits<int16>::max()
                )
            );
        }
        if (
            y < std::numeric_limits<int16>::min()
            || y > std::numeric_limits<int16>::max()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client y coordinate {} is outside the encodable range 0..={}",
                    y,
                    std::numeric_limits<int16>::max()
                )
            );
        }

        auto const narrowedX = static_cast<int16>(x);
        auto const narrowedY = static_cast<int16>(y);
        if (narrowedX < 0 || narrowedY < 0)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client coordinates must be non-negative, got {}x{}",
                    narrowedX,
                    narrowedY
                )
            );
        }

        return ClientPixel{narrowedX, narrowedY};
    }

    auto WheelDelta::create(int32 notches) -> Result<WheelDelta>
    {
        if (notches == 0)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "wheel delta must be a non-zero number of notches"
            );
        }
        if (notches < -k_maxWheelNotches || notches > k_maxWheelNotches)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "wheel delta of {} notches is outside the deliverable range -{}..={}",
                    notches,
                    k_maxWheelNotches,
                    k_maxWheelNotches
                )
            );
        }
        return WheelDelta{static_cast<int16>(notches)};
    }

    KeyInput::KeyInput(uint16 virtualKey) noexcept
        : m_virtualKey{virtualKey}
        , m_extended{controller_detail::isExtendedKey(virtualKey)}
    {
    }

    auto KeyInput::fromName(std::string_view name) -> Result<KeyInput>
    {
        UF_TRY_VALUE(validated, KeyName::create(name));
        return fromKeyName(validated);
    }

    auto KeyInput::fromKeyName(KeyName name) noexcept -> KeyInput
    {
        auto const text = name.value();
        if (auto const virtualKey = namedKeyVirtualKey(text))
        {
            return KeyInput{*virtualKey};
        }
        if (auto const number = functionKeyNumber(text))
        {
            return KeyInput{
                static_cast<uint16>(k_virtualKeyF1 + (*number - 1U))
            };
        }

        // KeyName admits only a named key, a function key, or exactly one
        // uppercase letter or digit, so this branch has one byte to read.
        // VK_A..VK_Z and VK_0..VK_9 are defined as the ASCII codes of the
        // uppercase letter and of the digit, so the character is the key code.
        UF_CHECK(text.size() == 1U);
        return KeyInput{static_cast<uint16>(text.front())};
    }
}

namespace uf::controller_detail
{
    auto keyboardLParamBits(
        uint8 scanCode,
        bool extended,
        KeyTransition transition
    ) noexcept -> uint32
    {
        auto bits = uint32{1U};
        bits |= static_cast<uint32>(scanCode) << 16U;
        if (extended)
        {
            bits |= uint32{1U} << 24U;
        }
        if (transition == KeyTransition::Up)
        {
            bits |= uint32{1U} << 30U;
            bits |= uint32{1U} << 31U;
        }
        return bits;
    }

    auto pointerLParamBits(ClientPixel pixel) noexcept -> uint32
    {
        return packPointBits(pixel.x(), pixel.y());
    }

    auto ScreenPixel::create(int64 x, int64 y) -> Result<ScreenPixel>
    {
        auto const narrowedX = checkedCast<int16>(x);
        auto const narrowedY = checkedCast<int16>(y);
        if (!narrowedX || !narrowedY)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "screen point ({}, {}) cannot be encoded as signed-16-bit mouse coordinates",
                    x,
                    y
                )
            );
        }
        return ScreenPixel{*narrowedX, *narrowedY};
    }

    auto screenPixelFor(ClientOrigin origin, ClientPixel pixel) -> Result<ScreenPixel>
    {
        // Widened before adding: two separately encodable coordinates need not
        // sum to an encodable one.
        return ScreenPixel::create(
            static_cast<int64>(origin.x) + static_cast<int64>(pixel.x()),
            static_cast<int64>(origin.y) + static_cast<int64>(pixel.y())
        );
    }

    auto isExtendedKey(uint16 virtualKey) noexcept -> bool
    {
        constexpr auto extended = std::array{
            uint16{0x00A3U},
            uint16{0x00A5U},
            uint16{0x002DU},
            uint16{0x002EU},
            uint16{0x0024U},
            uint16{0x0023U},
            uint16{0x0021U},
            uint16{0x0022U},
            uint16{0x0025U},
            uint16{0x0026U},
            uint16{0x0027U},
            uint16{0x0028U},
            uint16{0x0090U},
            uint16{0x0003U},
            uint16{0x002CU},
            uint16{0x006FU},
        };
        return std::ranges::find(extended, virtualKey) != extended.end();
    }

    auto keySpec(
        KeyInput key,
        uint8 scanCode,
        KeyTransition transition
    ) noexcept -> PostSpec
    {
        return PostSpec{
            .message = transition == KeyTransition::Down ? k_wmKeyDown : k_wmKeyUp,
            .wParam = key.virtualKey(),
            .lParam = lParamFromBits(
                keyboardLParamBits(scanCode, key.isExtended(), transition)
            ),
        };
    }

    auto pointerSpec(PointerMessage message, ClientPixel pixel) noexcept -> PostSpec
    {
        auto messageId = uint32{};
        auto wParam    = uintptr{};
        switch (message)
        {
        case PointerMessage::Move:
            messageId = k_wmMouseMove;
            break;
        case PointerMessage::MoveWithLeftButton:
            messageId = k_wmMouseMove;
            wParam    = k_leftButtonMask;
            break;
        case PointerMessage::LeftDown:
            messageId = k_wmLeftButtonDown;
            wParam    = k_leftButtonMask;
            break;
        case PointerMessage::LeftUp:
            messageId = k_wmLeftButtonUp;
            break;
        }

        return PostSpec{
            .message = messageId,
            .wParam  = wParam,
            .lParam  = lParamFromBits(pointerLParamBits(pixel)),
        };
    }

    auto wheelSpec(
        ScreenPixel pixel,
        WheelDelta delta,
        bool leftButtonHeld
    ) noexcept -> PostSpec
    {
        // wParam carries the signed raw delta in its high word and the button
        // and modifier state in its low word. Only the left button is reported
        // there, and that is now a stated gap rather than a consequence of the
        // key set: KeyName admits "SHIFT", so a held modifier has become
        // expressible. Reaching that state still needs keyDown, which has no
        // caller, so no wheel this project posts can be missing an MK_SHIFT it
        // should carry. Derive one here the day keyDown acquires a caller,
        // rather than from state nothing can currently produce.
        auto const rawDelta = static_cast<uintptr>(
            static_cast<uint16>(delta.rawUnits())
        );
        auto const buttons = leftButtonHeld ? k_leftButtonMask : uintptr{};
        return PostSpec{
            .message = k_wmMouseWheel,
            .wParam  = (rawDelta << 16U) | buttons,
            // Screen coordinates. Every other pointer message on this path packs
            // client coordinates, and posting those here would aim the wheel at
            // whatever sits that far from the desktop origin instead.
            .lParam  = lParamFromBits(packPointBits(pixel.x(), pixel.y())),
        };
    }

    auto charSpec(uint16 codeUnit) noexcept -> PostSpec
    {
        return PostSpec{
            .message = k_wmChar,
            .wParam  = codeUnit,
            .lParam  = 1,
        };
    }

    auto unicharSpec(char32_t codePoint) noexcept -> PostSpec
    {
        return PostSpec{
            .message = k_wmUnichar,
            .wParam  = static_cast<uintptr>(codePoint),
            .lParam  = 1,
        };
    }

    auto scanCodeFor(uint16 virtualKey) noexcept -> uint8
    {
        return controller_platform::scanCodeFor(virtualKey);
    }

    auto clientOriginOnScreen(WindowHandle windowHandle) -> Result<ClientOrigin>
    {
        return controller_platform::clientOriginOnScreen(windowHandle);
    }

    auto deliver(
        WindowHandle windowHandle,
        PostSpec spec,
        AuditLog& audit
    ) -> Status
    {
        return controller_platform::postInputMessage(windowHandle, spec, audit);
    }
}
