#include "sad.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>

#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace uf
{
    namespace
    {
        [[nodiscard]]
        auto checkedSubspan(
            std::span<std::byte const> data UF_LIFETIME_BOUND,
            std::size_t offset,
            std::size_t count
        ) noexcept -> std::optional<std::span<std::byte const>>
        {
            auto const end = checkedAdd(offset, count);
            if (!end || *end > data.size())
            {
                return std::nullopt;
            }

            auto const lastIndex = checkedSubtract(*end, std::size_t{1});
            if (
                count != 0
                && (
                    !lastIndex
                    || tryAt(data, *lastIndex) == nullptr
                )
            )
            {
                return std::nullopt;
            }

            return data.subspan(offset, count);
        }
    }

    auto GrayImage::create(
        std::span<std::byte const> data,
        uint32 width,
        uint32 height,
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
        uint64 best,
        uint64 maximumPixelComparisons,
        uint64 completedPixelComparisons,
        SadSearchPoll const& poll
    ) const -> CandidateReport
    {
        auto const templateWidth = checkedCast<std::size_t>(templateImage.m_width);
        auto const templateHeight = checkedCast<std::size_t>(templateImage.m_height);
        UF_CHECK(templateWidth.has_value());
        UF_CHECK(templateHeight.has_value());
        auto sum = uint64{0};

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
                if (completedPixelComparisons == maximumPixelComparisons)
                {
                    return CandidateReport{
                        SadSearchStopReason::ComparisonBudgetExhausted,
                        completedPixelComparisons
                    };
                }
                if (
                    completedPixelComparisons % g_sadSearchPollIntervalComparisons == 0
                )
                {
                    switch (poll())
                    {
                    case SadSearchControl::Continue:
                        break;
                    case SadSearchControl::Cancelled:
                        return CandidateReport{
                            SadSearchStopReason::Cancelled,
                            completedPixelComparisons
                        };
                    case SadSearchControl::TimedOut:
                        return CandidateReport{
                            SadSearchStopReason::TimedOut,
                            completedPixelComparisons
                        };
                    default:
                        UF_UNREACHABLE_MSG("Unknown SadSearchControl value");
                    }
                }
                ++completedPixelComparisons;

                auto const haystackPixel = std::to_integer<uint32>(
                    checkedAt(*haystackRow, templateX)
                );
                auto const templatePixel = std::to_integer<uint32>(
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
                return CandidateReport{sum, completedPixelComparisons};
            }
        }

        return CandidateReport{sum, completedPixelComparisons};
    }

    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        PixelRect roi
    ) -> Result<std::optional<SadMatch>>
    {
        auto const continueSearch = SadSearchPoll{
            []() noexcept -> SadSearchControl
            {
                return SadSearchControl::Continue;
            }
        };
        UF_TRY_VALUE(
            report,
            matchTemplateSad(
                haystack,
                templateImage,
                roi,
                std::numeric_limits<uint64>::max(),
                continueSearch
            )
        );
        auto const& outcome = report.m_outcome;
        UF_CHECK(std::holds_alternative<std::optional<SadMatch>>(outcome));
        return std::get<std::optional<SadMatch>>(outcome);
    }

    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>
    {
        UF_CHECK(poll != nullptr);
        UF_TRY(roi.ensureWithinExtent(haystack.width(), haystack.height()));

        if (
            templateImage.width() > roi.width()
            || templateImage.height() > roi.height()
        )
        {
            return SadSearchReport{
                SadSearchOutcome{std::optional<SadMatch>{}},
                0
            };
        }

        auto const lastX = checkedSubtract(roi.right(), templateImage.width());
        auto const lastY = checkedSubtract(roi.bottom(), templateImage.height());
        UF_CHECK(lastX.has_value());
        UF_CHECK(lastY.has_value());
        auto best                      = std::numeric_limits<uint64>::max();
        auto bestMatch                 = std::optional<SadMatch>{};
        auto completedPixelComparisons = uint64{0};

        for (auto candidateY = roi.y(); candidateY <= *lastY; ++candidateY)
        {
            for (auto candidateX = roi.x(); candidateX <= *lastX; ++candidateX)
            {
                auto const candidateXSize = checkedCast<std::size_t>(candidateX);
                auto const candidateYSize = checkedCast<std::size_t>(candidateY);
                UF_CHECK(candidateXSize.has_value());
                UF_CHECK(candidateYSize.has_value());
                auto const candidate = haystack.candidateSad(
                    templateImage,
                    *candidateXSize,
                    *candidateYSize,
                    best,
                    maximumPixelComparisons,
                    completedPixelComparisons,
                    poll
                );
                completedPixelComparisons = candidate.m_completedPixelComparisons;
                if (
                    auto const* reason = std::get_if<SadSearchStopReason>(
                        &candidate.m_outcome
                    )
                )
                {
                    return SadSearchReport{
                        SadSearchOutcome{*reason},
                        completedPixelComparisons
                    };
                }
                auto const score = std::get<uint64>(candidate.m_outcome);
                if (score < best)
                {
                    best = score;
                    bestMatch.emplace(candidateX, candidateY, score);
                    if (best == 0)
                    {
                        return SadSearchReport{
                            SadSearchOutcome{bestMatch},
                            completedPixelComparisons
                        };
                    }
                }
            }
        }

        return SadSearchReport{
            SadSearchOutcome{bestMatch},
            completedPixelComparisons
        };
    }

    auto bgra8ToGray8(
        std::span<std::byte const> bgra,
        uint32 width,
        uint32 height,
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
                auto const blue = std::to_integer<uint32>(
                    checkedAt(*row, *pixelOffset)
                );
                auto const green = std::to_integer<uint32>(
                    checkedAt(*row, *greenOffset)
                );
                auto const red = std::to_integer<uint32>(
                    checkedAt(*row, *redOffset)
                );
                auto const weightedGray = (
                    uint32{77} * red
                    + uint32{150} * green
                    + uint32{29} * blue
                );
                auto const gray = weightedGray >> 8;
                auto const grayByte = checkedCast<uint8>(gray);
                UF_CHECK(grayByte.has_value());
                output.emplace_back(std::byte{*grayByte});
            }
        }

        return output;
    }
}
