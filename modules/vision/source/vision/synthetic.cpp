#include "synthetic.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

namespace uf
{
    auto hashedGray(
        uint32 seed,
        uint32 x,
        uint32 y
    ) noexcept -> uint8
    {
        auto value = x * uint32{0x9E37'79B1};
        value += y * uint32{0x85EB'CA77};
        value += seed * uint32{0x27D4'EB2F};
        value ^= value >> 15;
        value *= uint32{0x2C1B'3C6D};
        value ^= value >> 13;
        auto const lowByte = checkedCast<uint8>(value & uint32{0xFF});
        UF_CHECK(lowByte.has_value());
        return *lowByte;
    }
}
