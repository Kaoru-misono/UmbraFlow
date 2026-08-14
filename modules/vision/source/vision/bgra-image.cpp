#include "bgra-image.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>

#include <image/pixels.hpp>

#include <cstddef>
#include <format>
#include <span>

namespace uf
{
    namespace
    {
        constexpr auto k_bgraBytesPerPixel = std::size_t{4};
    }

    auto BgraImage::create(
        std::span<std::byte const> data,
        uint32 width,
        uint32 height,
        std::size_t stride
    ) -> Result<BgraImage>
    {
        auto const widthSize = checkedCast<std::size_t>(width);
        auto const heightSize = checkedCast<std::size_t>(height);
        if (!widthSize || !heightSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("bgra image dimensions {}x{} do not fit buffer geometry", width, height)
            );
        }

        auto const minimumRow = checkedMultiply(*widthSize, k_bgraBytesPerPixel);
        if (!minimumRow)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("bgra row size overflow: width {} * 4", width)
            );
        }

        UF_TRY(
            validateBufferGeometry(
                width,
                height,
                stride,
                *minimumRow,
                data.size()
            )
        );

        if (!checkedMultiply(*widthSize, *heightSize))
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("bgra plane size overflow: width {} * height {}", width, height)
            );
        }

        return BgraImage{data, width, height, stride};
    }

    auto BgraImage::pixelCount() const noexcept -> std::size_t
    {
        auto const width = checkedCast<std::size_t>(m_width);
        auto const height = checkedCast<std::size_t>(m_height);
        UF_CHECK(width.has_value());
        UF_CHECK(height.has_value());
        auto const total = checkedMultiply(*width, *height);
        UF_CHECK(total.has_value());
        return *total;
    }

    auto BgraImage::pixelAt(uint32 x, uint32 y) const noexcept -> Bgra8Pixel
    {
        UF_CHECK(x < m_width);
        UF_CHECK(y < m_height);

        auto const rowStart = checkedMultiply(std::size_t{y}, m_stride);
        UF_CHECK(rowStart.has_value());
        auto const pixelOffset = checkedMultiply(std::size_t{x}, k_bgraBytesPerPixel);
        UF_CHECK(pixelOffset.has_value());
        auto const base = checkedAdd(*rowStart, *pixelOffset);
        UF_CHECK(base.has_value());
        auto const last = checkedAdd(*base, k_bgraBytesPerPixel - 1U);
        UF_CHECK(last.has_value());
        UF_CHECK(*last < m_data.size());

        return Bgra8Pixel{
            .blue  = std::to_integer<uint8>(checkedAt(m_data, *base)),
            .green = std::to_integer<uint8>(checkedAt(m_data, *base + 1U)),
            .red   = std::to_integer<uint8>(checkedAt(m_data, *base + 2U)),
            .alpha = std::to_integer<uint8>(checkedAt(m_data, *base + 3U)),
        };
    }

    auto BgraImage::grayAt(uint32 x, uint32 y) const noexcept -> uint8
    {
        auto const pixel = pixelAt(x, y);
        return image::rgb8ToGray8(pixel.red, pixel.green, pixel.blue);
    }
}
