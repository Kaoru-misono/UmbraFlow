#pragma once

#include "controller/input.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <string_view>
#include <vector>

namespace uf::controller_detail
{
    inline constexpr auto g_wmKeyDown = uint32{0x0100U};
    inline constexpr auto g_wmKeyUp = uint32{0x0101U};
    inline constexpr auto g_wmChar = uint32{0x0102U};
    inline constexpr auto g_wmUnichar = uint32{0x0109U};
    inline constexpr auto g_wmMouseMove = uint32{0x0200U};
    inline constexpr auto g_wmLeftButtonDown = uint32{0x0201U};
    inline constexpr auto g_wmLeftButtonUp = uint32{0x0202U};
    inline constexpr auto g_leftButtonMask = uintptr{0x0001U};

    enum class KeyTransition : uint8
    {
        Down,
        Up,
    };

    enum class PointerMessage : uint8
    {
        Move,
        MoveWithLeftButton,
        LeftDown,
        LeftUp,
    };

    struct PostSpec final
    {
        uint32  m_message;
        uintptr m_wParam;
        intptr  m_lParam;

        auto operator==(PostSpec const&) const -> bool = default;
    };

    [[nodiscard]]
    auto keyboardLParamBits(
        uint8 scanCode,
        bool extended,
        KeyTransition transition
    ) noexcept -> uint32;

    [[nodiscard]] auto pointerLParamBits(ClientPixel pixel) noexcept -> uint32;
    [[nodiscard]] auto isExtendedKey(uint16 virtualKey) noexcept -> bool;

    [[nodiscard]]
    auto keySpec(
        KeyInput key,
        uint8 scanCode,
        KeyTransition transition
    ) noexcept -> PostSpec;

    [[nodiscard]]
    auto pointerSpec(PointerMessage message, ClientPixel pixel) noexcept -> PostSpec;

    [[nodiscard]] auto charSpec(uint16 codeUnit) noexcept -> PostSpec;
    [[nodiscard]] auto unicharSpec(char32_t codePoint) noexcept -> PostSpec;
    [[nodiscard]]
    auto utf16CodeUnits(std::string_view text) -> Result<std::vector<uint16>>;
    [[nodiscard]] auto scanCodeFor(uint16 virtualKey) noexcept -> uint8;

    [[nodiscard]]
    auto deliver(
        WindowHandle windowHandle,
        PostSpec spec,
        AuditLog& audit
    ) -> Status;
}
