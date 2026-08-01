#pragma once

#include "error.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace uf
{
    // The content address of a blob: a task's compiled source, a template PNG,
    // a project file a script read or wrote. It is a value rather than a string
    // so two hashes compare as digests and a malformed one cannot be built.
    //
    // It lives in domain rather than in core because its failure model is
    // domain's: a hash that cannot be parsed is an InvalidResource, and core
    // declares no link dependencies and therefore owns no error vocabulary. It
    // lived in modules/annotation until 2026-08-01, where it was only ever a
    // tenant -- nothing about hashing is about annotating.
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
