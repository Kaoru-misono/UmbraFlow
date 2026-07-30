#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <span>

namespace uf
{
    // One BGRA8 pixel split into channels. Alpha is carried through because a
    // template PNG's alpha channel is the matcher's mask plane, so a caller
    // that reads pixels to build one needs it.
    struct Bgra8Pixel final
    {
        uint8 blue{};
        uint8 green{};
        uint8 red{};
        uint8 alpha{};

        auto operator==(Bgra8Pixel const&) const -> bool = default;
    };

    // A read-only BGRA8 plane view. The backing storage must outlive this
    // object and every call that reads through it.
    //
    // This is the one place the module validates BGRA8 buffer geometry and the
    // one place it defines what grey a colour pixel has; bgra8ToGray8 and the
    // frame-analysis primitives all read through it rather than walking a
    // buffer themselves.
    class BgraImage final
    {
        std::span<std::byte const> m_data;
        uint32                     m_width;
        uint32                     m_height;
        std::size_t                m_stride;

        constexpr BgraImage(
            std::span<std::byte const> data,
            uint32 width,
            uint32 height,
            std::size_t stride
        ) noexcept
            : m_data{data}
            , m_width{width}
            , m_height{height}
            , m_stride{stride}
        {
        }

    public:
        [[nodiscard]]
        static auto create(
            std::span<std::byte const> data UF_LIFETIME_BOUND,
            uint32 width,
            uint32 height,
            std::size_t stride
        ) -> Result<BgraImage>;

        [[nodiscard]] constexpr auto width() const noexcept -> uint32 { return m_width; }
        [[nodiscard]] constexpr auto height() const noexcept -> uint32 { return m_height; }
        [[nodiscard]] constexpr auto stride() const noexcept -> std::size_t { return m_stride; }

        // Pixels in the plane, which is width times height and never overflows
        // because create rejected the geometry that would.
        [[nodiscard]] auto pixelCount() const noexcept -> std::size_t;

        // x must be below width() and y below height(). Both are checked in
        // release: an out-of-range read here is a caller bug, not input.
        [[nodiscard]] auto pixelAt(uint32 x, uint32 y) const noexcept -> Bgra8Pixel;

        // The module's single grey definition, the BT.601 integer weights
        // bgra8ToGray8 applies. Same pixel, same byte, by construction rather
        // than by two constant tables agreeing.
        [[nodiscard]] auto grayAt(uint32 x, uint32 y) const noexcept -> uint8;
    };
}
