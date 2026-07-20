#pragma once

#include "controller/input.hpp"

#include <core/error/result.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace uf::controller_detail
{
    inline constexpr auto wmKeyDown = std::uint32_t{0x0100U};
    inline constexpr auto wmKeyUp = std::uint32_t{0x0101U};
    inline constexpr auto wmChar = std::uint32_t{0x0102U};
    inline constexpr auto wmUnichar = std::uint32_t{0x0109U};
    inline constexpr auto wmMouseMove = std::uint32_t{0x0200U};
    inline constexpr auto wmLeftButtonDown = std::uint32_t{0x0201U};
    inline constexpr auto wmLeftButtonUp = std::uint32_t{0x0202U};
    inline constexpr auto leftButtonMask = std::uintptr_t{0x0001U};

    enum class KeyTransition : std::uint8_t
    {
        Down,
        Up,
    };

    enum class PointerMessage : std::uint8_t
    {
        Move,
        MoveWithLeftButton,
        LeftDown,
        LeftUp,
    };

    struct PostSpec final
    {
        std::uint32_t m_message;
        std::uintptr_t m_wParam;
        std::intptr_t m_lParam;

        auto operator==(PostSpec const&) const -> bool = default;
    };

    [[nodiscard]]
    auto keyboardLParamBits(
        std::uint8_t scanCode,
        bool extended,
        KeyTransition transition
    ) noexcept -> std::uint32_t;

    [[nodiscard]] auto pointerLParamBits(ClientPixel pixel) noexcept -> std::uint32_t;
    [[nodiscard]] auto isExtendedKey(std::uint16_t virtualKey) noexcept -> bool;

    [[nodiscard]]
    auto keySpec(
        KeyInput key,
        std::uint8_t scanCode,
        KeyTransition transition
    ) noexcept -> PostSpec;

    [[nodiscard]]
    auto pointerSpec(PointerMessage message, ClientPixel pixel) noexcept -> PostSpec;

    [[nodiscard]] auto charSpec(std::uint16_t codeUnit) noexcept -> PostSpec;
    [[nodiscard]] auto unicharSpec(char32_t codePoint) noexcept -> PostSpec;
    [[nodiscard]]
    auto utf16CodeUnits(std::string_view text) -> Result<std::vector<std::uint16_t>>;
    [[nodiscard]] auto scanCodeFor(std::uint16_t virtualKey) noexcept -> std::uint8_t;

    [[nodiscard]]
    auto deliver(
        WindowHandle windowHandle,
        PostSpec spec,
        AuditLog& audit
    ) -> Status;
}
