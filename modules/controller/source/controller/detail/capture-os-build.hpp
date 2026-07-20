#pragma once

#include <cstdint>

namespace uf::controller_detail
{
    inline constexpr auto g_cursorCaptureMinBuild = std::uint32_t{19'041};
    inline constexpr auto g_borderlessMinBuild = std::uint32_t{20'348};

    [[nodiscard]]
    constexpr auto cursorCaptureSupported(std::uint32_t build) noexcept -> bool
    {
        return build >= g_cursorCaptureMinBuild;
    }

    [[nodiscard]]
    constexpr auto borderlessSupported(std::uint32_t build) noexcept -> bool
    {
        return build >= g_borderlessMinBuild;
    }
}
