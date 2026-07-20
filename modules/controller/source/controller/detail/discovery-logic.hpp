#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace uf::controller_detail
{
    [[nodiscard]]
    constexpr auto fileTimeToTicks(
        std::uint32_t high,
        std::uint32_t low
    ) noexcept -> std::uint64_t
    {
        return (static_cast<std::uint64_t>(high) << 32U) | low;
    }

    [[nodiscard]]
    auto utf16BufferToString(
        std::span<char16_t const> buffer,
        std::int32_t length
    ) -> std::string;

    [[nodiscard]]
    auto utf16BufferToPath(
        std::span<char16_t const> buffer,
        std::int32_t length
    ) -> std::filesystem::path;
}
