#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace uf::annotation
{
    class ContentHash final
    {
        std::array<uint8, 32> m_bytes;

        constexpr explicit ContentHash(std::array<uint8, 32> bytes) noexcept
            : m_bytes{bytes}
        {
        }

        friend auto sha256(
            std::span<std::byte const> bytes
        ) -> Result<ContentHash>;

    public:
        auto operator<=>(ContentHash const&) const = default;

        [[nodiscard]]
        static auto parse(std::string_view value) -> Result<ContentHash>;

        [[nodiscard]] auto hex() const -> std::string;
        [[nodiscard]] auto toString() const -> std::string;

        [[nodiscard]]
        auto bytes() const noexcept UF_LIFETIME_BOUND -> std::span<uint8 const>;
    };

    [[nodiscard]]
    auto sha256(
        std::span<std::byte const> bytes
    ) -> Result<ContentHash>;
}
