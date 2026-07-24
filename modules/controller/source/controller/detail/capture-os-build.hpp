#pragma once

#include <core/types/integer.hpp>

namespace uf::controller_detail
{
    inline constexpr auto k_cursorCaptureMinBuild = uint32{19'041};
    inline constexpr auto k_borderlessMinBuild = uint32{20'348};

    [[nodiscard]]
    constexpr auto cursorCaptureSupported(uint32 build) noexcept -> bool
    {
        return build >= k_cursorCaptureMinBuild;
    }

    [[nodiscard]]
    constexpr auto borderlessSupported(uint32 build) noexcept -> bool
    {
        return build >= k_borderlessMinBuild;
    }
}
