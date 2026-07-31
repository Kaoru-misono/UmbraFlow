#pragma once

#include "controller/input.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <string_view>
#include <vector>

namespace uf::controller_detail
{
    inline constexpr auto k_wmKeyDown = uint32{0x0100U};
    inline constexpr auto k_wmKeyUp = uint32{0x0101U};
    inline constexpr auto k_wmChar = uint32{0x0102U};
    inline constexpr auto k_wmUnichar = uint32{0x0109U};
    inline constexpr auto k_wmMouseMove = uint32{0x0200U};
    inline constexpr auto k_wmLeftButtonDown = uint32{0x0201U};
    inline constexpr auto k_wmLeftButtonUp = uint32{0x0202U};
    inline constexpr auto k_wmMouseWheel = uint32{0x020AU};
    inline constexpr auto k_leftButtonMask = uintptr{0x0001U};

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
        uint32  message{};
        uintptr wParam{};
        intptr  lParam{};

        auto operator==(PostSpec const&) const -> bool = default;
    };

    // A window client area's top-left corner in screen coordinates. Only the
    // wheel path needs it: WM_MOUSEWHEEL's lParam is documented in screen
    // coordinates, while WM_MOUSEMOVE and the button messages are in client
    // coordinates and take no offset at all.
    struct ClientOrigin final
    {
        int32 x{};
        int32 y{};

        auto operator==(ClientOrigin const&) const -> bool = default;
    };

    // A pixel in screen space, deliberately not a ClientPixel: a screen
    // coordinate is negative on any monitor left of or above the primary one,
    // which ClientPixel refuses by construction. Two types also stop a message
    // builder from packing whichever space it was handed.
    class ScreenPixel final
    {
        int16 m_x;
        int16 m_y;

        constexpr ScreenPixel(int16 x, int16 y) noexcept
            : m_x{x}
            , m_y{y}
        {
        }

    public:
        auto operator<=>(ScreenPixel const&) const = default;

        [[nodiscard]]
        static auto create(int64 x, int64 y) -> Result<ScreenPixel>;

        [[nodiscard]] constexpr auto x() const noexcept -> int32 { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> int32 { return m_y; }
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

    // ClientToScreen is a pure translation by the client origin, so a client
    // pixel's screen pixel is that origin plus the pixel. Fails when the sum
    // leaves the range a mouse message can encode.
    [[nodiscard]]
    auto screenPixelFor(ClientOrigin origin, ClientPixel pixel) -> Result<ScreenPixel>;

    [[nodiscard]]
    auto wheelSpec(
        ScreenPixel pixel,
        WheelDelta delta,
        bool leftButtonHeld
    ) noexcept -> PostSpec;

    [[nodiscard]] auto charSpec(uint16 codeUnit) noexcept -> PostSpec;
    [[nodiscard]] auto unicharSpec(char32_t codePoint) noexcept -> PostSpec;
    [[nodiscard]]
    auto utf16CodeUnits(std::string_view text) -> Result<std::vector<uint16>>;
    [[nodiscard]] auto scanCodeFor(uint16 virtualKey) noexcept -> uint8;
    [[nodiscard]]
    auto clientOriginOnScreen(WindowHandle windowHandle) -> Result<ClientOrigin>;

    [[nodiscard]]
    auto deliver(
        WindowHandle windowHandle,
        PostSpec spec,
        AuditLog& audit
    ) -> Status;
}
