#pragma once

#include <core/types/integer.hpp>

#include <bit>

namespace uf::text_unsafe
{
    [[nodiscard]]
    constexpr auto utf8CodeUnitValue(char codeUnit) noexcept -> uint8
    {
        static_assert(sizeof(char) == sizeof(uint8));
        // SAFETY: UTF-8 treats char storage as an eight-bit code unit. bit_cast copies
        // that object representation without numeric sign conversion or aliasing.
        return std::bit_cast<uint8>(codeUnit);
    }

    [[nodiscard]]
    constexpr auto utf8CodeUnit(uint8 value) noexcept -> char
    {
        static_assert(sizeof(char) == sizeof(uint8));
        // SAFETY: Every uint8 bit pattern is a valid char object representation;
        // bit_cast preserves the UTF-8 code-unit bits without narrowing.
        return std::bit_cast<char>(value);
    }

    [[nodiscard]]
    constexpr auto utf16CodeUnitValue(char16_t codeUnit) noexcept -> uint16
    {
        static_assert(sizeof(char16_t) == sizeof(uint16));
        // SAFETY: char16_t stores one unsigned UTF-16 code unit. bit_cast exposes the
        // same sixteen bits as their numeric value without a representation cast.
        return std::bit_cast<uint16>(codeUnit);
    }
}
