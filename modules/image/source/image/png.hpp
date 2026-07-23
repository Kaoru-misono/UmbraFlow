#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

namespace uf::image
{
    inline constexpr auto g_maximumPngDimension = uint32{8192};
    inline constexpr auto g_maximumPngPixels = std::size_t{8192} * 8192U;
    inline constexpr auto g_maximumPngFileBytes = std::size_t{64} * 1024U * 1024U;

    struct RgbaImage final
    {
        uint32                 m_width{};
        uint32                 m_height{};
        std::vector<std::byte> m_pixels{};
    };

    [[nodiscard]]
    auto decodePng(
        std::span<std::byte const> encoded,
        std::string_view resourceName
    ) -> Result<RgbaImage>;

    [[nodiscard]]
    auto loadPng(std::filesystem::path const& path) -> Result<RgbaImage>;

    [[nodiscard]]
    auto encodeRgbaPng(
        std::string_view resourceName,
        uint32 width,
        uint32 height,
        std::span<std::byte const> pixels
    ) -> Result<std::vector<std::byte>>;

    [[nodiscard]]
    auto writeRgbaPng(
        std::filesystem::path const& path,
        uint32 width,
        uint32 height,
        std::span<std::byte const> pixels
    ) -> Status;
}
