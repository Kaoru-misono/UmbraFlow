#include "template-cut.hpp"

#include "pixels.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace uf::image
{
    namespace
    {
        constexpr auto k_rgbaBytesPerPixel = std::size_t{4};

        [[nodiscard]]
        auto invalidCut(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        [[nodiscard]]
        auto requiredRgbaBytes(uint32 width, uint32 height)
            -> std::optional<std::size_t>
        {
            auto const widthSize  = checkedCast<std::size_t>(width);
            auto const heightSize = checkedCast<std::size_t>(height);
            auto const pixels = widthSize && heightSize
                ? checkedMultiply(*widthSize, *heightSize)
                : std::optional<std::size_t>{};
            return pixels
                ? checkedMultiply(*pixels, k_rgbaBytesPerPixel)
                : std::optional<std::size_t>{};
        }

        [[nodiscard]]
        auto cropRgba8(RgbaImage const& source, PixelRect rect)
            -> Result<std::vector<std::byte>>
        {
            UF_TRY(rect.ensureWithinExtent(source.width, source.height));
            auto const sourceBytes = requiredRgbaBytes(
                source.width,
                source.height
            );
            if (!sourceBytes || source.pixels.size() != *sourceBytes)
            {
                return invalidCut(
                    std::format(
                        "RGBA8 source storage does not match {}x{}",
                        source.width,
                        source.height
                    )
                );
            }

            auto const sourceWidth = checkedCast<std::size_t>(source.width);
            auto const x           = checkedCast<std::size_t>(rect.x());
            auto const y           = checkedCast<std::size_t>(rect.y());
            auto const width       = checkedCast<std::size_t>(rect.width());
            auto const height      = checkedCast<std::size_t>(rect.height());
            UF_CHECK(sourceWidth.has_value());
            UF_CHECK(x.has_value());
            UF_CHECK(y.has_value());
            UF_CHECK(width.has_value());
            UF_CHECK(height.has_value());

            auto const sourceStride = checkedMultiply(
                *sourceWidth,
                k_rgbaBytesPerPixel
            );
            auto const rowBytes = checkedMultiply(
                *width,
                k_rgbaBytesPerPixel
            );
            auto const totalBytes = rowBytes
                ? checkedMultiply(*rowBytes, *height)
                : std::optional<std::size_t>{};
            UF_CHECK(sourceStride.has_value());
            UF_CHECK(rowBytes.has_value());
            UF_CHECK(totalBytes.has_value());

            auto cropped     = std::vector<std::byte>(*totalBytes);
            auto croppedSpan = std::span<std::byte>{cropped};
            for (auto row = std::size_t{0}; row < *height; ++row)
            {
                auto const sourceRow = (*y + row) * *sourceStride;
                auto const sourceStart = sourceRow + *x * k_rgbaBytesPerPixel;
                auto const destinationStart = row * *rowBytes;
                auto const sourceSlice = std::span<std::byte const>{source.pixels}
                    .subspan(sourceStart, *rowBytes);
                std::ranges::copy(
                    sourceSlice,
                    croppedSpan.subspan(destinationStart, *rowBytes).begin()
                );
            }
            return cropped;
        }

        [[nodiscard]]
        auto grayAt(
            std::span<std::byte const> rgba,
            std::size_t pixelIndex
        ) noexcept -> uint8
        {
            auto const offset = pixelIndex * k_rgbaBytesPerPixel;
            return rgb8ToGray8(
                std::to_integer<uint8>(checkedAt(rgba, offset)),
                std::to_integer<uint8>(checkedAt(rgba, offset + 1U)),
                std::to_integer<uint8>(checkedAt(rgba, offset + 2U))
            );
        }
    }

    auto cutRgba8Template(
        std::span<RgbaImage const> sources,
        PixelRect rect
    ) -> Result<TemplateCut>
    {
        if (sources.empty())
        {
            return invalidCut("template cut requires at least one source image");
        }

        auto crops = std::vector<std::vector<std::byte>>{};
        crops.reserve(sources.size());
        auto const sourceWidth  = sources.front().width;
        auto const sourceHeight = sources.front().height;
        for (auto const& source : sources)
        {
            if (
                source.width != sourceWidth
                || source.height != sourceHeight
            )
            {
                return invalidCut(
                    std::format(
                        "template cut source extent {}x{} does not match {}x{}",
                        source.width,
                        source.height,
                        sourceWidth,
                        sourceHeight
                    )
                );
            }
            UF_TRY_VALUE(crop, cropRgba8(source, rect));
            crops.emplace_back(std::move(crop));
        }

        auto const pixelCount = crops.front().size() / k_rgbaBytesPerPixel;
        if (sources.size() == 1U)
        {
            auto output = std::move(crops.front());
            for (auto index = std::size_t{0}; index < pixelCount; ++index)
            {
                checkedAt(output, index * k_rgbaBytesPerPixel + 3U)
                    = std::byte{255};
            }
            return TemplateCut{
                .image = RgbaImage{
                    .width  = rect.width(),
                    .height = rect.height(),
                    .pixels = std::move(output),
                },
                .alphaDerivation = TemplateAlphaDerivation::SingleSourceOpaque,
            };
        }

        auto spreads       = std::vector<uint8>(pixelCount);
        auto maximumSpread = uint8{0};
        for (auto index = std::size_t{0}; index < pixelCount; ++index)
        {
            auto minimumGray = std::numeric_limits<uint8>::max();
            auto maximumGray = uint8{0};
            for (auto const& crop : crops)
            {
                auto const gray = grayAt(crop, index);
                minimumGray = std::min(minimumGray, gray);
                maximumGray = std::max(maximumGray, gray);
            }
            auto const spread = static_cast<uint8>(maximumGray - minimumGray);
            checkedAt(spreads, index) = spread;
            maximumSpread = std::max(maximumSpread, spread);
        }

        auto output = std::move(crops.front());
        // Full weight is reserved for a pixel observed not to move at all.
        // Anchoring the scale at zero rather than at the smallest observed
        // spread keeps that meaning fixed: a capture that disturbs the ground
        // more can never promote a pixel that was itself seen to move.
        auto const spreadRange = static_cast<uint32>(maximumSpread);
        for (auto index = std::size_t{0}; index < pixelCount; ++index)
        {
            auto weight = uint8{255};
            if (spreadRange != 0U)
            {
                auto const inverse = static_cast<uint32>(
                    maximumSpread - checkedAt(spreads, index)
                );
                auto const rounded = (
                    inverse * uint32{255}
                    + spreadRange / 2U
                ) / spreadRange;
                weight = static_cast<uint8>(rounded);
            }
            checkedAt(output, index * k_rgbaBytesPerPixel + 3U)
                = static_cast<std::byte>(weight);
        }

        return TemplateCut{
            .image = RgbaImage{
                .width  = rect.width(),
                .height = rect.height(),
                .pixels = std::move(output),
            },
            .alphaDerivation = TemplateAlphaDerivation::ObservedSpread,
        };
    }
}
