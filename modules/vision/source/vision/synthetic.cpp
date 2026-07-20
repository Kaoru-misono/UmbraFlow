#include "synthetic.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-cast.hpp>

#include <cstdint>

namespace uf
{
    auto hashedGray(
        std::uint32_t seed,
        std::uint32_t x,
        std::uint32_t y
    ) noexcept -> std::uint8_t
    {
        auto value = x * std::uint32_t{0x9E37'79B1};
        value += y * std::uint32_t{0x85EB'CA77};
        value += seed * std::uint32_t{0x27D4'EB2F};
        value ^= value >> 15;
        value *= std::uint32_t{0x2C1B'3C6D};
        value ^= value >> 13;
        auto const lowByte = checkedCast<std::uint8_t>(value & std::uint32_t{0xFF});
        UF_CHECK(lowByte.has_value());
        return *lowByte;
    }
}
