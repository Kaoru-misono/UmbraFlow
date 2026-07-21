#pragma once

#include <core/types/integer.hpp>

#include <filesystem>
#include <span>
#include <string>

namespace uf::controller_detail
{
    [[nodiscard]]
    constexpr auto fileTimeToTicks(
        uint32 high,
        uint32 low
    ) noexcept -> uint64
    {
        return (static_cast<uint64>(high) << 32U) | low;
    }

    [[nodiscard]]
    auto utf16BufferToString(
        std::span<char16_t const> buffer,
        int32 length
    ) -> std::string;

    [[nodiscard]]
    auto utf16BufferToPath(
        std::span<char16_t const> buffer,
        int32 length
    ) -> std::filesystem::path;
}
