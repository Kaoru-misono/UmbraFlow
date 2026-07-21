#include "windows-background-messages.hpp"

#include <core/types/integer.hpp>

#include <Windows.h>

#include <array>
#include <ranges>

namespace uf::m0_demo::platform
{
    auto isAllowedBackgroundMessage(uint32 message) noexcept -> bool
    {
        auto constexpr allowed = std::array<uint32, 7>{
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
