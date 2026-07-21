#pragma once

#include <core/types/integer.hpp>

namespace uf
{
    [[nodiscard]]
    auto hashedGray(
        uint32 seed,
        uint32 x,
        uint32 y
    ) noexcept -> uint8;
}
