#pragma once

#include <cstdint>

namespace uf
{
    [[nodiscard]]
    auto hashedGray(
        std::uint32_t seed,
        std::uint32_t x,
        std::uint32_t y
    ) noexcept -> std::uint8_t;
}
