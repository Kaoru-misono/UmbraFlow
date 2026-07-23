#include "pixels.hpp"

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>
#include <domain/error.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::image
{
    namespace
    {
        constexpr auto g_bgraBytesPerPixel = std::size_t{4};

        [[nodiscard]]
        auto swapRedBlueChannels(
            std::vector<std::byte> pixels,
            std::string_view layout
        ) -> Result<std::vector<std::byte>>
        {
            if (pixels.size() % g_bgraBytesPerPixel != 0U)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "{} pixel bytes are not a whole number of 4-byte pixels",
                        layout
                    )
                );
            }

            for (
                auto index = std::size_t{0};
                index < pixels.size();
                index += g_bgraBytesPerPixel
            )
            {
                std::swap(
                    checkedAt(pixels, index),
                    checkedAt(pixels, index + 2U)
                );
            }
            return pixels;
        }
    }

    auto rgba8ToBgra8(
        std::vector<std::byte> rgba
    ) -> Result<std::vector<std::byte>>
    {
        return swapRedBlueChannels(std::move(rgba), "RGBA8");
    }

    auto bgra8ToRgba8(
        std::vector<std::byte> bgra
    ) -> Result<std::vector<std::byte>>
    {
        return swapRedBlueChannels(std::move(bgra), "BGRA8");
    }

    auto cropBgra8(
        std::span<std::byte const> source,
        uint32 sourceWidth,
        uint32 sourceHeight,
        std::size_t stride,
        PixelRect rect
    ) -> Result<std::vector<std::byte>>
    {
        if (rect.right() > sourceWidth || rect.bottom() > sourceHeight)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "BGRA8 crop ({}, {}, {}, {}) is outside source extent {}x{}",
                    rect.x(),
                    rect.y(),
                    rect.width(),
                    rect.height(),
                    sourceWidth,
                    sourceHeight
                )
            );
        }

        auto const sourceWidthSize = checkedCast<std::size_t>(sourceWidth);
        auto const sourceHeightSize = checkedCast<std::size_t>(sourceHeight);
        auto const sourceRowBytes = sourceWidthSize
            ? checkedMultiply(*sourceWidthSize, g_bgraBytesPerPixel)
            : std::optional<std::size_t>{};
        auto const finalRow = sourceHeightSize && *sourceHeightSize > 0U
            ? std::optional<std::size_t>{*sourceHeightSize - 1U}
            : std::optional<std::size_t>{};
        auto const finalRowOffset = finalRow
            ? checkedMultiply(*finalRow, stride)
            : std::optional<std::size_t>{};
        auto const requiredSourceBytes = finalRowOffset && sourceRowBytes
            ? checkedAdd(*finalRowOffset, *sourceRowBytes)
            : std::optional<std::size_t>{};
        if (
            !sourceRowBytes
            || !requiredSourceBytes
            || stride < *sourceRowBytes
            || source.size() < *requiredSourceBytes
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::format(
                    "BGRA8 source storage does not match {}x{} with stride {}",
                    sourceWidth,
                    sourceHeight,
                    stride
                )
            );
        }

        auto const x = checkedCast<std::size_t>(rect.x());
        auto const y = checkedCast<std::size_t>(rect.y());
        auto const width = checkedCast<std::size_t>(rect.width());
        auto const height = checkedCast<std::size_t>(rect.height());
        if (!x || !y || !width || !height)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "cropped BGRA8 rectangle is not addressable"
            );
        }
        auto const xBytes = checkedMultiply(*x, g_bgraBytesPerPixel);
        auto const rowBytes = checkedMultiply(*width, g_bgraBytesPerPixel);
        auto const totalBytes = rowBytes
            ? checkedMultiply(*rowBytes, *height)
            : std::optional<std::size_t>{};
        if (!xBytes || !rowBytes || !totalBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "cropped BGRA8 byte geometry overflowed"
            );
        }

        auto output     = std::vector<std::byte>(*totalBytes);
        auto outputSpan = std::span<std::byte>{output};
        for (auto row = std::size_t{0}; row < *height; ++row)
        {
            auto const sourceY = checkedAdd(*y, row);
            auto const rowOffset = sourceY
                ? checkedMultiply(*sourceY, stride)
                : std::optional<std::size_t>{};
            auto const sourceStart = rowOffset
                ? checkedAdd(*rowOffset, *xBytes)
                : std::optional<std::size_t>{};
            auto const sourceEnd = sourceStart
                ? checkedAdd(*sourceStart, *rowBytes)
                : std::optional<std::size_t>{};
            auto const destinationStart = checkedMultiply(row, *rowBytes);
            if (
                !sourceStart
                || !sourceEnd
                || !destinationStart
                || *sourceEnd > source.size()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::format(
                        "cropped BGRA8 row {} exceeds source buffer of {} bytes",
                        row,
                        source.size()
                    )
                );
            }

            auto const sourceSlice = source.subspan(*sourceStart, *rowBytes);
            auto destinationSlice = outputSpan.subspan(*destinationStart, *rowBytes);
            std::ranges::copy(sourceSlice, destinationSlice.begin());
        }
        return output;
    }
}
