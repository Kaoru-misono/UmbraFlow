#include "sad.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>

#include <domain/frame.hpp>

#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace
{
    [[nodiscard]]
    auto checkedSubspan(
        std::span<std::byte const> data UF_LIFETIME_BOUND,
        std::size_t offset,
        std::size_t count
    ) noexcept -> std::optional<std::span<std::byte const>>
    {
        auto const end = uf::checkedAdd(offset, count);
        if (!end || *end > data.size())
        {
            return std::nullopt;
        }

        auto const lastIndex = uf::checkedSubtract(*end, std::size_t{1});
        if (
            count != 0
            && (
                !lastIndex
                || uf::tryAt(data, *lastIndex) == nullptr
            )
        )
        {
            return std::nullopt;
        }

        return data.subspan(offset, count);
    }
}

namespace uf
{
    auto GrayImage::create(
        std::span<std::byte const> data,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride
    ) -> Result<GrayImage>
    {
        auto const widthSize = checkedCast<std::size_t>(width);
        if (!widthSize)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("gray image width {} does not fit buffer geometry", width)
            );
        }

        UF_TRY(
            validateBufferGeometry(
                width,
                height,
                stride,
                *widthSize,
                data.size()
            )
        );

        return GrayImage{data, width, height, stride};
    }

    auto GrayImage::rowSegment(
        std::size_t y,
        std::size_t x,
        std::size_t width
    ) const noexcept -> std::optional<std::span<std::byte const>>
    {
        auto const imageWidth = checkedCast<std::size_t>(m_width);
        auto const imageHeight = checkedCast<std::size_t>(m_height);
        UF_CHECK(imageWidth.has_value());
        UF_CHECK(imageHeight.has_value());
        auto const xEnd = checkedAdd(x, width);
        if (
            y >= *imageHeight
            || !xEnd
            || *xEnd > *imageWidth
        )
        {
            return std::nullopt;
        }

        auto const rowOffset = checkedMultiply(y, m_stride);
        if (!rowOffset)
        {
            return std::nullopt;
        }

        auto const segmentOffset = checkedAdd(*rowOffset, x);
        if (!segmentOffset)
        {
            return std::nullopt;
        }

        return checkedSubspan(m_data, *segmentOffset, width);
    }

    auto GrayImage::candidateSad(
        GrayImage const& templateImage,
        std::size_t candidateX,
        std::size_t candidateY,
        std::uint64_t best
    ) const noexcept -> std::uint64_t
    {
        auto const templateWidth = checkedCast<std::size_t>(templateImage.m_width);
        auto const templateHeight = checkedCast<std::size_t>(templateImage.m_height);
        UF_CHECK(templateWidth.has_value());
        UF_CHECK(templateHeight.has_value());
        auto sum = std::uint64_t{0};

        for (auto templateY = std::size_t{0}; templateY < *templateHeight; ++templateY)
        {
            auto const haystackY = checkedAdd(candidateY, templateY);
            UF_CHECK(haystackY.has_value());

            auto const haystackRow = rowSegment(
                *haystackY,
                candidateX,
                *templateWidth
            );
            auto const templateRow = templateImage.rowSegment(
                templateY,
                0,
                *templateWidth
            );
            UF_CHECK(haystackRow.has_value());
            UF_CHECK(templateRow.has_value());

            for (auto templateX = std::size_t{0}; templateX < *templateWidth; ++templateX)
            {
                auto const haystackPixel = std::to_integer<std::uint32_t>(
                    checkedAt(*haystackRow, templateX)
                );
                auto const templatePixel = std::to_integer<std::uint32_t>(
                    checkedAt(*templateRow, templateX)
                );
                auto const difference = (
                    haystackPixel >= templatePixel
                        ? haystackPixel - templatePixel
                        : templatePixel - haystackPixel
                );
                sum += difference;
            }

            if (sum >= best)
            {
                return sum;
            }
        }

        return sum;
    }

    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        PixelRect roi
    ) -> Result<std::optional<SadMatch>>
    {
        UF_TRY(roi.ensureWithinExtent(haystack.width(), haystack.height()));

        if (
            templateImage.width() > roi.width()
            || templateImage.height() > roi.height()
        )
        {
            return std::optional<SadMatch>{};
        }

        auto const lastX = checkedSubtract(roi.right(), templateImage.width());
        auto const lastY = checkedSubtract(roi.bottom(), templateImage.height());
        UF_CHECK(lastX.has_value());
        UF_CHECK(lastY.has_value());
        auto best = std::numeric_limits<std::uint64_t>::max();
        auto bestMatch = std::optional<SadMatch>{};

        for (auto candidateY = roi.y(); candidateY <= *lastY; ++candidateY)
        {
            for (auto candidateX = roi.x(); candidateX <= *lastX; ++candidateX)
            {
                auto const candidateXSize = checkedCast<std::size_t>(candidateX);
                auto const candidateYSize = checkedCast<std::size_t>(candidateY);
                UF_CHECK(candidateXSize.has_value());
                UF_CHECK(candidateYSize.has_value());
                auto const score = haystack.candidateSad(
                    templateImage,
                    *candidateXSize,
                    *candidateYSize,
                    best
                );
                if (score < best)
                {
                    best = score;
                    bestMatch.emplace(candidateX, candidateY, score);
                    if (best == 0)
                    {
                        return bestMatch;
                    }
                }
            }
        }

        return bestMatch;
    }

    auto bgra8ToGray8(
        std::span<std::byte const> bgra,
        std::uint32_t width,
        std::uint32_t height,
        std::size_t stride
    ) -> Result<std::vector<std::byte>>
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

        auto const minimumRow = checkedMultiply(*widthSize, std::size_t{4});
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
                bgra.size()
            )
        );

        auto const outputLength = checkedMultiply(*widthSize, *heightSize);
        if (!outputLength)
        {
            return fail(
                AutomationErrorKind::InternalInvariant,
                std::format("gray image size overflow: width {} * height {}", width, height)
            );
        }

        auto output = std::vector<std::byte>{};
        output.reserve(*outputLength);
        for (auto y = std::size_t{0}; y < *heightSize; ++y)
        {
            auto const rowStart = checkedMultiply(y, stride);
            UF_CHECK(rowStart.has_value());

            auto const row = checkedSubspan(bgra, *rowStart, *minimumRow);
            UF_CHECK(row.has_value());

            for (auto x = std::size_t{0}; x < *widthSize; ++x)
            {
                auto const pixelOffset = checkedMultiply(x, std::size_t{4});
                UF_CHECK(pixelOffset.has_value());
                auto const greenOffset = checkedAdd(*pixelOffset, std::size_t{1});
                auto const redOffset = checkedAdd(*pixelOffset, std::size_t{2});
                UF_CHECK(greenOffset.has_value());
                UF_CHECK(redOffset.has_value());
                auto const blue = std::to_integer<std::uint32_t>(
                    checkedAt(*row, *pixelOffset)
                );
                auto const green = std::to_integer<std::uint32_t>(
                    checkedAt(*row, *greenOffset)
                );
                auto const red = std::to_integer<std::uint32_t>(
                    checkedAt(*row, *redOffset)
                );
                auto const weightedGray = (
                    std::uint32_t{77} * red
                    + std::uint32_t{150} * green
                    + std::uint32_t{29} * blue
                );
                auto const gray = weightedGray >> 8;
                auto const grayByte = checkedCast<std::uint8_t>(gray);
                UF_CHECK(grayByte.has_value());
                output.emplace_back(std::byte{*grayByte});
            }
        }

        return output;
    }
}
