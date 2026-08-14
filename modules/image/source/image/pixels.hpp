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
    constexpr auto rgb8ToGray8(uint8 red, uint8 green, uint8 blue) noexcept -> uint8
    {
        auto const weighted = (
            uint32{77} * red
            + uint32{150} * green
            + uint32{29} * blue
        );
        return static_cast<uint8>(weighted >> 8U);
    }

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
