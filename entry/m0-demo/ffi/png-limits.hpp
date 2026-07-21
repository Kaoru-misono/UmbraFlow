#pragma once

#include <core/types/integer.hpp>

#include <cstddef>

namespace uf::m0_demo::ffi
{
    inline constexpr auto g_maximumPngDimension = uint32{8192};
    inline constexpr auto g_maximumPngPixels = std::size_t{8192} * 8192U;
}
