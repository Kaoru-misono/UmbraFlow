#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace uf::m0_demo::ffi
{
    inline constexpr auto g_maximumTemplateDimension = std::uint32_t{8192};
    inline constexpr auto g_maximumTemplatePixels = std::size_t{8192} * 8192U;
    inline constexpr auto g_maximumTemplateFileBytes = std::size_t{64} * 1024U * 1024U;

    struct RgbaImage final
    {
        std::uint32_t m_width;
        std::uint32_t m_height;
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
