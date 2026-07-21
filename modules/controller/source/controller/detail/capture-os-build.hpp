#pragma once

#include <core/types/integer.hpp>

namespace uf::controller_detail
{
    inline constexpr auto g_cursorCaptureMinBuild = uint32{19'041};
    inline constexpr auto g_borderlessMinBuild = uint32{20'348};

    [[nodiscard]]
    constexpr auto cursorCaptureSupported(uint32 build) noexcept -> bool
    {
        return build >= g_cursorCaptureMinBuild;
    }

    [[nodiscard]]
    constexpr auto borderlessSupported(uint32 build) noexcept -> bool
    {
        return build >= g_borderlessMinBuild;
    }
}
