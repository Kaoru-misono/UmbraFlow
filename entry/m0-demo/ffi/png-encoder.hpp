#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace uf::m0_demo::ffi
{
    [[nodiscard]]
    auto encodeRgbaPng(
        std::filesystem::path const& path,
        std::uint32_t width,
        std::uint32_t height,
        std::span<std::byte const> pixels
    ) -> Result<std::vector<std::byte>>;

    [[nodiscard]]
    auto writeRgbaPng(
        std::filesystem::path const& path,
        std::uint32_t width,
        std::uint32_t height,
        std::span<std::byte const> pixels
    ) -> Status;
}
