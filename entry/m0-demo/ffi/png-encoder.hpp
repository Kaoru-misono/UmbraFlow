#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <filesystem>
#include <span>
#include <vector>

namespace uf::m0_demo::ffi
{
    [[nodiscard]]
    auto encodeRgbaPng(
        std::filesystem::path const& path,
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
