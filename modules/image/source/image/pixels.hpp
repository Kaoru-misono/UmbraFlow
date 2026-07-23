#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <domain/space.hpp>

#include <cstddef>
#include <span>
#include <vector>

namespace uf::image
{
    [[nodiscard]]
    auto rgba8ToBgra8(
        std::vector<std::byte> rgba
    ) -> Result<std::vector<std::byte>>;

    [[nodiscard]]
    auto bgra8ToRgba8(
        std::vector<std::byte> bgra
    ) -> Result<std::vector<std::byte>>;

    [[nodiscard]]
    auto cropBgra8(
        std::span<std::byte const> source,
        uint32 sourceWidth,
        uint32 sourceHeight,
        std::size_t stride,
        PixelRect rect
    ) -> Result<std::vector<std::byte>>;
}
