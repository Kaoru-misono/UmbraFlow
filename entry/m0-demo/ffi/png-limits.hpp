#pragma once

#include <cstddef>
#include <cstdint>

namespace uf::m0_demo::ffi
{
    inline constexpr auto g_maximumPngDimension = std::uint32_t{8192};
    inline constexpr auto g_maximumPngPixels = std::size_t{8192} * 8192U;
}
