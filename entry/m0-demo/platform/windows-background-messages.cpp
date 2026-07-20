#include "windows-background-messages.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <ranges>

namespace uf::m0_demo::platform
{
    auto isAllowedBackgroundMessage(std::uint32_t message) noexcept -> bool
    {
        auto constexpr allowed = std::array<std::uint32_t, 7>{
            WM_MOUSEMOVE,
            WM_LBUTTONDOWN,
            WM_LBUTTONUP,
            WM_KEYDOWN,
            WM_KEYUP,
            WM_CHAR,
            WM_UNICHAR,
        };
        return std::ranges::find(allowed, message) != allowed.end();
    }
}
