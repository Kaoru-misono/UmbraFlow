#pragma once

#include "png-limits.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace uf::m0_demo::ffi
{
    inline constexpr auto g_maximumTemplateDimension = g_maximumPngDimension;
    inline constexpr auto g_maximumTemplatePixels = g_maximumPngPixels;
    inline constexpr auto g_maximumTemplateFileBytes = std::size_t{64} * 1024U * 1024U;

    struct RgbaImage final
    {
        uint32 m_width;
        uint32 m_height;
        std::vector<std::byte> m_pixels;
    };

    [[nodiscard]]
    auto decodePng(
        std::span<std::byte const> encoded,
        std::string_view resourceName
    ) -> Result<RgbaImage>;

    [[nodiscard]]
    auto loadPng(std::filesystem::path const& path) -> Result<RgbaImage>;
}
