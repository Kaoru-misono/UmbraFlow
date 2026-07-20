#include "detail/input-message.hpp"

#include "platform/windows-controller.hpp"

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <limits>

namespace
{
    [[nodiscard]]
    constexpr auto lParamFromBits(std::uint32_t bits) noexcept -> std::intptr_t
    {
        static_assert(sizeof(std::intptr_t) >= sizeof(std::uint32_t));
        if constexpr (sizeof(std::intptr_t) == sizeof(std::uint32_t))
        {
            return std::bit_cast<std::int32_t>(bits);
        }
        else
        {
            return static_cast<std::intptr_t>(bits);
        }
    }
}

namespace uf
{
    auto ClientPixel::create(std::int32_t x, std::int32_t y) -> Result<ClientPixel>
    {
        if (
            x < std::numeric_limits<std::int16_t>::min()
            || x > std::numeric_limits<std::int16_t>::max()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client x coordinate {} is outside the encodable range 0..={}",
                    x,
                    std::numeric_limits<std::int16_t>::max()
                )
            );
        }
        if (
            y < std::numeric_limits<std::int16_t>::min()
            || y > std::numeric_limits<std::int16_t>::max()
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "client y coordinate {} is outside the encodable range 0..={}",
                    y,
                    std::numeric_limits<std::int16_t>::max()
                )
            );
        }

        auto const narrowedX = static_cast<std::int16_t>(x);
        auto const narrowedY = static_cast<std::int16_t>(y);
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

    KeyInput::KeyInput(std::uint16_t virtualKey) noexcept
        : m_virtualKey{virtualKey}
        , m_extended{controller_detail::isExtendedKey(virtualKey)}
    {
    }
}

namespace uf::controller_detail
{
    auto keyboardLParamBits(
        std::uint8_t scanCode,
        bool extended,
        KeyTransition transition
    ) noexcept -> std::uint32_t
    {
        auto bits = std::uint32_t{1U};
        bits |= static_cast<std::uint32_t>(scanCode) << 16U;
        if (extended)
        {
            bits |= std::uint32_t{1U} << 24U;
        }
        if (transition == KeyTransition::Up)
        {
            bits |= std::uint32_t{1U} << 30U;
            bits |= std::uint32_t{1U} << 31U;
        }
        return bits;
    }

    auto pointerLParamBits(ClientPixel pixel) noexcept -> std::uint32_t
    {
        auto const low = static_cast<std::uint32_t>(
            static_cast<std::uint16_t>(pixel.x())
        );
        auto const high = static_cast<std::uint32_t>(
            static_cast<std::uint16_t>(pixel.y())
        );
        return (high << 16U) | low;
    }

    auto isExtendedKey(std::uint16_t virtualKey) noexcept -> bool
    {
        constexpr auto extended = std::array{
            std::uint16_t{0x00A3U},
            std::uint16_t{0x00A5U},
            std::uint16_t{0x002DU},
            std::uint16_t{0x002EU},
            std::uint16_t{0x0024U},
            std::uint16_t{0x0023U},
            std::uint16_t{0x0021U},
            std::uint16_t{0x0022U},
            std::uint16_t{0x0025U},
            std::uint16_t{0x0026U},
            std::uint16_t{0x0027U},
            std::uint16_t{0x0028U},
            std::uint16_t{0x0090U},
            std::uint16_t{0x0003U},
            std::uint16_t{0x002CU},
            std::uint16_t{0x006FU},
        };
        return std::ranges::find(extended, virtualKey) != extended.end();
    }

    auto keySpec(
        KeyInput key,
        std::uint8_t scanCode,
        KeyTransition transition
    ) noexcept -> PostSpec
    {
        return PostSpec{
            .m_message = transition == KeyTransition::Down ? wmKeyDown : wmKeyUp,
            .m_wParam = key.virtualKey(),
            .m_lParam = lParamFromBits(
                keyboardLParamBits(scanCode, key.isExtended(), transition)
            ),
        };
    }

    auto pointerSpec(PointerMessage message, ClientPixel pixel) noexcept -> PostSpec
    {
        auto messageId = std::uint32_t{};
        auto wParam = std::uintptr_t{};
        switch (message)
        {
        case PointerMessage::Move:
            messageId = wmMouseMove;
            break;
        case PointerMessage::MoveWithLeftButton:
            messageId = wmMouseMove;
            wParam = leftButtonMask;
            break;
        case PointerMessage::LeftDown:
            messageId = wmLeftButtonDown;
            wParam = leftButtonMask;
            break;
        case PointerMessage::LeftUp:
            messageId = wmLeftButtonUp;
            break;
        }

        return PostSpec{
            .m_message = messageId,
            .m_wParam = wParam,
            .m_lParam = lParamFromBits(pointerLParamBits(pixel)),
        };
    }

    auto charSpec(std::uint16_t codeUnit) noexcept -> PostSpec
    {
        return PostSpec{
            .m_message = wmChar,
            .m_wParam = codeUnit,
            .m_lParam = 0,
        };
    }

    auto unicharSpec(char32_t codePoint) noexcept -> PostSpec
    {
        return PostSpec{
            .m_message = wmUnichar,
            .m_wParam = static_cast<std::uintptr_t>(codePoint),
            .m_lParam = 0,
        };
    }

    auto scanCodeFor(std::uint16_t virtualKey) noexcept -> std::uint8_t
    {
        return controller_platform::scanCodeFor(virtualKey);
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
