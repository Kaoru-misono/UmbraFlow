#include "detail/input-message.hpp"

#include "platform/windows-controller.hpp"

#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <limits>

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
    }
}

namespace uf
{
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

    KeyInput::KeyInput(uint16 virtualKey) noexcept
        : m_virtualKey{virtualKey}
        , m_extended{controller_detail::isExtendedKey(virtualKey)}
    {
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
        auto const low = static_cast<uint32>(
            static_cast<uint16>(pixel.x())
        );
        auto const high = static_cast<uint32>(
            static_cast<uint16>(pixel.y())
        );
        return (high << 16U) | low;
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
            .m_message = transition == KeyTransition::Down ? k_wmKeyDown : k_wmKeyUp,
            .m_wParam = key.virtualKey(),
            .m_lParam = lParamFromBits(
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
            .m_message = messageId,
            .m_wParam  = wParam,
            .m_lParam  = lParamFromBits(pointerLParamBits(pixel)),
        };
    }

    auto charSpec(uint16 codeUnit) noexcept -> PostSpec
    {
        return PostSpec{
            .m_message = k_wmChar,
            .m_wParam  = codeUnit,
            .m_lParam  = 1,
        };
    }

    auto unicharSpec(char32_t codePoint) noexcept -> PostSpec
    {
        return PostSpec{
            .m_message = k_wmUnichar,
            .m_wParam  = static_cast<uintptr>(codePoint),
            .m_lParam  = 1,
        };
    }

    auto scanCodeFor(uint16 virtualKey) noexcept -> uint8
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
