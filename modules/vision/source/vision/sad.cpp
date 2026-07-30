#include "sad.hpp"

#include "bgra-image.hpp"

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
        // The largest weighted absolute difference one pixel can contribute.
        constexpr auto k_maximumPixelContribution = uint64{255} * 255U;

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

            return data.subspan(offset, count);
        }

        // Rescales a weighted sum onto the unmasked score's scale. Reducing the
        // quotient when the weights already sum to the pixel count is exact
        // rather than an approximation, and it keeps the unmasked path free of
        // the multiplication that the masked path bounds at entry.
        [[nodiscard]]
        auto normalizedScore(
            uint64 weightedSum,
            uint64 templatePixels,
            uint64 totalWeight
        ) noexcept -> uint64
        {
            if (totalWeight == templatePixels)
            {
                return weightedSum;
            }

            auto const scaled = checkedMultiply(weightedSum, templatePixels);
            UF_CHECK(scaled.has_value());
            return *scaled / totalWeight;
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

    auto GrayImage::weightSum() const noexcept -> uint64
    {
        auto const width = checkedCast<std::size_t>(m_width);
        auto const height = checkedCast<std::size_t>(m_height);
        UF_CHECK(width.has_value());
        UF_CHECK(height.has_value());
        auto total = uint64{0};

        for (auto y = std::size_t{0}; y < *height; ++y)
        {
            auto const row = rowSegment(y, 0, *width);
            UF_CHECK(row.has_value());
            for (auto const value : *row)
            {
                total += std::to_integer<uint64>(value);
            }
        }

        return total;
    }

    auto GrayImage::candidateSad(
        GrayImage const& templateImage,
        GrayImage const* p_templateMask,
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
            auto const maskRow = (
                p_templateMask != nullptr
                    ? p_templateMask->rowSegment(templateY, 0, *templateWidth)
                    : std::optional<std::span<std::byte const>>{}
            );
            UF_CHECK(p_templateMask == nullptr || maskRow.has_value());

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
                    completedPixelComparisons % k_sadSearchPollIntervalComparisons == 0
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

                // An excluded pixel contributes nothing, so its two byte reads
                // and its difference are pure waste. Skipping only the work,
                // never the counter, keeps the budget measuring the rectangle
                // the search actually walked.
                auto const weight = (
                    maskRow
                        ? std::to_integer<uint64>(checkedAt(*maskRow, templateX))
                        : uint64{1}
                );
                if (weight == 0)
                {
                    continue;
                }

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
                sum += weight * uint64{difference};
            }

            if (sum >= best)
            {
                return CandidateReport{sum, completedPixelComparisons};
            }
        }

        return CandidateReport{sum, completedPixelComparisons};
    }

    auto GrayImage::search(
        GrayImage const& templateImage,
        GrayImage const* p_templateMask,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) const -> Result<SadSearchReport>
    {
        UF_CHECK(poll != nullptr);
        UF_TRY(roi.ensureWithinExtent(m_width, m_height));

        auto const templatePixels = checkedMultiply(
            uint64{templateImage.m_width},
            uint64{templateImage.m_height}
        );
        UF_CHECK(templatePixels.has_value());
        auto totalWeight = *templatePixels;
        if (p_templateMask != nullptr)
        {
            if (
                p_templateMask->m_width != templateImage.m_width
                || p_templateMask->m_height != templateImage.m_height
            )
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "template mask {}x{} does not match template {}x{}",
                        p_templateMask->m_width,
                        p_templateMask->m_height,
                        templateImage.m_width,
                        templateImage.m_height
                    )
                );
            }

            // The score multiplies a weighted sum of at most
            // k_maximumPixelContribution per pixel by the pixel count again, so
            // bounding that product here lets every later step stay unchecked.
            auto const worstSum = checkedMultiply(
                *templatePixels,
                k_maximumPixelContribution
            );
            auto const worstScaled = (
                worstSum
                    ? checkedMultiply(*worstSum, *templatePixels)
                    : std::optional<uint64>{}
            );
            if (!worstScaled)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    std::format(
                        "template {}x{} is too large to normalize a masked score",
                        templateImage.m_width,
                        templateImage.m_height
                    )
                );
            }

            totalWeight = p_templateMask->weightSum();
            if (totalWeight == 0)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "template mask excludes every pixel of its template"
                );
            }
        }

        if (
            templateImage.m_width > roi.width()
            || templateImage.m_height > roi.height()
        )
        {
            return SadSearchReport{
                SadSearchOutcome{std::optional<SadMatch>{}},
                0
            };
        }

        auto const lastX = checkedSubtract(roi.right(), templateImage.m_width);
        auto const lastY = checkedSubtract(roi.bottom(), templateImage.m_height);
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
                auto const candidate = candidateSad(
                    templateImage,
                    p_templateMask,
                    *candidateXSize,
                    *candidateYSize,
                    best,
                    maximumPixelComparisons,
                    completedPixelComparisons,
                    poll
                );
                completedPixelComparisons = candidate.completedPixelComparisons;
                if (
                    auto const* reason = std::get_if<SadSearchStopReason>(
                        &candidate.outcome
                    )
                )
                {
                    return SadSearchReport{
                        SadSearchOutcome{*reason},
                        completedPixelComparisons
                    };
                }
                // Ranking stays on the weighted sums because the normalization
                // factor is one constant for the whole search, so pruning and
                // the exact-match exit read the same order the reported scores
                // do while avoiding a division per candidate.
                auto const score = std::get<uint64>(candidate.outcome);
                if (score < best)
                {
                    best = score;
                    bestMatch.emplace(
                        candidateX,
                        candidateY,
                        normalizedScore(score, *templatePixels, totalWeight)
                    );
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
        auto const& outcome = report.outcome;
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
        return haystack.search(
            templateImage,
            nullptr,
            roi,
            maximumPixelComparisons,
            poll
        );
    }

    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        GrayImage const& templateMask,
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
                templateMask,
                roi,
                std::numeric_limits<uint64>::max(),
                continueSearch
            )
        );
        auto const& outcome = report.outcome;
        UF_CHECK(std::holds_alternative<std::optional<SadMatch>>(outcome));
        return std::get<std::optional<SadMatch>>(outcome);
    }

    auto matchTemplateSad(
        GrayImage const& haystack,
        GrayImage const& templateImage,
        GrayImage const& templateMask,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>
    {
        return haystack.search(
            templateImage,
            &templateMask,
            roi,
            maximumPixelComparisons,
            poll
        );
    }

    auto bgra8ToGray8(
        std::span<std::byte const> bgra,
        uint32 width,
        uint32 height,
        std::size_t stride
    ) -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(
            plane,
            BgraImage::create(bgra, width, height, stride)
        );

        auto output = std::vector<std::byte>{};
        output.reserve(plane.pixelCount());
        for (auto y = uint32{0}; y < height; ++y)
        {
            for (auto x = uint32{0}; x < width; ++x)
            {
                output.emplace_back(std::byte{plane.grayAt(x, y)});
            }
        }

        return output;
    }

    auto bgra8ToAlpha8(
        std::span<std::byte const> bgra,
        uint32 width,
        uint32 height,
        std::size_t stride
    ) -> Result<std::vector<std::byte>>
    {
        UF_TRY_VALUE(
            plane,
            BgraImage::create(bgra, width, height, stride)
        );

        auto output = std::vector<std::byte>{};
        output.reserve(plane.pixelCount());
        for (auto y = uint32{0}; y < height; ++y)
        {
            for (auto x = uint32{0}; x < width; ++x)
            {
                output.emplace_back(std::byte{plane.pixelAt(x, y).alpha});
            }
        }

        return output;
    }
}
