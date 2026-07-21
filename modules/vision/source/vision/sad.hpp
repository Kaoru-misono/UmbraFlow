#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace uf
{
    class SadMatch final
    {
        std::uint32_t m_x;
        std::uint32_t m_y;
        std::uint64_t m_score;

    public:
        constexpr SadMatch(
            std::uint32_t x,
            std::uint32_t y,
            std::uint64_t score
        ) noexcept
            : m_x{x}
            , m_y{y}
            , m_score{score}
        {
        }

        auto operator==(SadMatch const&) const -> bool = default;

        [[nodiscard]] constexpr auto x() const noexcept -> std::uint32_t { return m_x; }
        [[nodiscard]] constexpr auto y() const noexcept -> std::uint32_t { return m_y; }
        [[nodiscard]] constexpr auto score() const noexcept -> std::uint64_t { return m_score; }
    };

    class GrayImage;

    [[nodiscard]]
    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        PixelRect roi
    ) -> Result<std::optional<SadMatch>>;

    // A read-only Gray8 view. The backing storage must outlive this object and
    // every matcher call that uses it.
    class GrayImage final
    {
        friend auto matchTemplateSad(
            GrayImage const& haystack,
            GrayImage const& templateImage,
            PixelRect roi
        ) -> Result<std::optional<SadMatch>>;

        std::span<std::byte const> m_data;
        std::uint32_t m_width;
        std::uint32_t m_height;
        std::size_t m_stride;

        constexpr GrayImage(
            std::span<std::byte const> data,
            std::uint32_t width,
            std::uint32_t height,
            std::size_t stride
        ) noexcept
            : m_data{data}
            , m_width{width}
            , m_height{height}
            , m_stride{stride}
        {
        }

        [[nodiscard]]
        auto rowSegment(
            std::size_t y,
            std::size_t x,
            std::size_t width
        ) const noexcept UF_LIFETIME_BOUND -> std::optional<std::span<std::byte const>>;

        [[nodiscard]]
        auto candidateSad(
            GrayImage const& templateImage,
            std::size_t candidateX,
            std::size_t candidateY,
            std::uint64_t best
        ) const noexcept -> std::uint64_t;

    public:
        [[nodiscard]]
        static auto create(
            std::span<std::byte const> data UF_LIFETIME_BOUND,
            std::uint32_t width,
            std::uint32_t height,
            std::size_t stride
        ) -> Result<GrayImage>;

        [[nodiscard]] constexpr auto width() const noexcept -> std::uint32_t { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> std::uint32_t { return m_height; }
        [[nodiscard]] constexpr auto stride() const noexcept -> std::size_t { return m_stride; }
    };

    [[nodiscard]]
    auto bgra8ToGray8(
        std::span<std::byte const> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride
    ) -> Result<std::vector<std::byte>>;
}
