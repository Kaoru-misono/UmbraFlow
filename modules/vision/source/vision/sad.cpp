#include "sad.hpp"

#include "bgra-image.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>
#include <core/utility/variant-match.hpp>

#include <domain/frame.hpp>

#include <algorithm>
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

        // How many positions outside the mask one probe reads per candidate.
        //
        // Sixteen, thirty-two, sixty-four, a hundred and twenty-eight and the
        // whole complement were measured against the consuming project's battle
        // corpus and agreed on the contrast ratio to within about one percent,
        // because the complement of a colour key is large and homogeneous.
        // Thirty-two is the smallest of those with room to spare: it costs a
        // masked candidate under a tenth of the comparisons its score already
        // spends, and it costs a candidate it refuses nothing else at all.
        constexpr auto k_maskContrastSamples = std::size_t{32};

        // The share of its template's own outside-mask contrast a candidate must
        // still show, as the divisor of one. See GrayImage::MaskContrastProbe
        // for the rule and for the measurement that chose this figure.
        constexpr auto k_maskContrastDivisor = uint64{2};

        [[nodiscard]]
        auto absoluteDifference(uint64 left, uint64 right) noexcept -> uint64
        {
            return left >= right ? left - right : right - left;
        }

        // The stop that ends a search at the comparison about to be executed, if
        // any. Both the contrast probe and the score walk the same counter
        // through here, so one budget and one poll cadence govern the whole
        // candidate however it is refused.
        [[nodiscard]]
        auto searchStopAt(
            uint64 completedPixelComparisons,
            uint64 maximumPixelComparisons,
            SadSearchPoll const& poll
        ) -> std::optional<SadSearchStopReason>
        {
            if (completedPixelComparisons == maximumPixelComparisons)
            {
                return SadSearchStopReason::ComparisonBudgetExhausted;
            }
            if (completedPixelComparisons % k_sadSearchPollIntervalComparisons != 0)
            {
                return std::nullopt;
            }

            switch (poll())
            {
            case SadSearchControl::Continue: return std::nullopt;
            case SadSearchControl::Cancelled: return SadSearchStopReason::Cancelled;
            case SadSearchControl::TimedOut: return SadSearchStopReason::TimedOut;
            }

            UF_UNREACHABLE_MSG("Unknown SadSearchControl value");
        }

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

    auto GrayImage::MaskContrastProbe::create(
        GrayImage const& templateImage,
        GrayImage const& templateMask
    ) -> MaskContrastProbe
    {
        auto const width  = checkedCast<std::size_t>(templateImage.m_width);
        auto const height = checkedCast<std::size_t>(templateImage.m_height);
        UF_CHECK(width.has_value());
        UF_CHECK(height.has_value());

        // The glyph the mask selected, as one grey. Truncating division, so the
        // same mask bytes give the same reference value everywhere.
        auto weightedInk = uint64{0};
        auto totalWeight = uint64{0};

        // Every position the mask did not take in full, row major. It is the
        // enumeration the sample positions are cut out of, and it is a property
        // of the template's extent and its mask alone -- no clock, no address
        // and no random source enters it, so one template always probes the same
        // positions, on every host and in every replay.
        auto complement = std::vector<Sample>{};

        for (auto y = std::size_t{0}; y < *height; ++y)
        {
            auto const templateRow = templateImage.rowSegment(y, 0, *width);
            auto const maskRow     = templateMask.rowSegment(y, 0, *width);
            UF_CHECK(templateRow.has_value());
            UF_CHECK(maskRow.has_value());

            for (auto x = std::size_t{0}; x < *width; ++x)
            {
                auto const weight = std::to_integer<uint64>(
                    checkedAt(*maskRow, x)
                );
                auto const pixel = std::to_integer<uint64>(
                    checkedAt(*templateRow, x)
                );
                weightedInk += weight * pixel;
                totalWeight += weight;
                if (weight == 255U)
                {
                    continue;
                }

                complement.emplace_back(
                    Sample{
                        .offsetX          = x,
                        .offsetY          = y,
                        .complementWeight = 255U - weight,
                    }
                );
            }
        }

        UF_CHECK(totalWeight != 0);
        auto probe  = MaskContrastProbe{};
        probe.m_ink = weightedInk / totalWeight;
        if (complement.empty())
        {
            return probe;
        }

        // One position per stratum, taken at the stratum's mid-point: with N
        // complement positions and K samples, sample j is enumeration index
        // (2j + 1) * N / (2K). It is the deterministic reading of "sample the
        // pixels outside the mask" -- an even spread through the complement
        // rather than a roll -- and because the strata are equal, the samples
        // land across the whole rectangle however the complement is shaped.
        auto const population = complement.size();
        auto const wanted     = std::min(population, k_maskContrastSamples);
        probe.m_samples.reserve(wanted);
        for (auto index = std::size_t{0}; index < wanted; ++index)
        {
            auto const position = ((2U * index + 1U) * population) / (2U * wanted);
            probe.m_samples.emplace_back(checkedAt(complement, position));
        }

        auto required = uint64{0};
        for (auto const& sample : probe.m_samples)
        {
            auto const templateRow = templateImage.rowSegment(
                sample.offsetY,
                sample.offsetX,
                1
            );
            UF_CHECK(templateRow.has_value());
            auto const pixel = std::to_integer<uint64>(
                checkedAt(*templateRow, 0)
            );
            auto const distance = absoluteDifference(pixel, probe.m_ink);
            required += sample.complementWeight * distance;
        }
        probe.m_requiredContrast = required / k_maskContrastDivisor;

        return probe;
    }

    auto GrayImage::candidateSad(
        GrayImage const& templateImage,
        GrayImage const* p_templateMask,
        MaskContrastProbe const& probe,
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

        // The negative evidence first: a candidate the mask separated nothing
        // in is refused here, and the score below is never spent on it.
        auto contrast = uint64{0};
        for (auto const& sample : probe.samples())
        {
            if (
                auto const stop = searchStopAt(
                    completedPixelComparisons,
                    maximumPixelComparisons,
                    poll
                )
            )
            {
                return CandidateReport{*stop, completedPixelComparisons};
            }
            ++completedPixelComparisons;

            auto const haystackY = checkedAdd(candidateY, sample.offsetY);
            auto const haystackX = checkedAdd(candidateX, sample.offsetX);
            UF_CHECK(haystackY.has_value());
            UF_CHECK(haystackX.has_value());
            auto const haystackPixel = rowSegment(*haystackY, *haystackX, 1);
            UF_CHECK(haystackPixel.has_value());
            auto const pixel = std::to_integer<uint64>(
                checkedAt(*haystackPixel, 0)
            );
            auto const distance = absoluteDifference(pixel, probe.ink());
            contrast += sample.complementWeight * distance;
        }
        if (contrast < probe.requiredContrast())
        {
            return CandidateReport{ContrastRefused{}, completedPixelComparisons};
        }

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
                if (
                    auto const stop = searchStopAt(
                        completedPixelComparisons,
                        maximumPixelComparisons,
                        poll
                    )
                )
                {
                    return CandidateReport{*stop, completedPixelComparisons};
                }
                ++completedPixelComparisons;

                // An excluded pixel contributes nothing to the SCORE, so its two
                // byte reads and its difference are pure waste here. Skipping
                // only the work, never the counter, keeps the budget measuring
                // the rectangle the search actually walked. What an excluded
                // pixel does contribute is negative evidence, and the probe
                // above has already read a fixed sample of it.
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
                .outcome                   = SadSearchOutcome{std::optional<SadMatch>{}},
                .completedPixelComparisons = 0,
                .contrastRefusedCandidates = 0,
            };
        }

        // Taken once for the whole search: the sample positions and the
        // requirement come from the template's extent and its mask, and neither
        // moves as the window slides.
        auto const probe = (
            p_templateMask != nullptr
                ? MaskContrastProbe::create(templateImage, *p_templateMask)
                : MaskContrastProbe{}
        );

        auto const lastX = checkedSubtract(roi.right(), templateImage.m_width);
        auto const lastY = checkedSubtract(roi.bottom(), templateImage.m_height);
        UF_CHECK(lastX.has_value());
        UF_CHECK(lastY.has_value());
        auto best                      = std::numeric_limits<uint64>::max();
        auto bestMatch                 = std::optional<SadMatch>{};
        auto completedPixelComparisons = uint64{0};
        auto contrastRefusedCandidates = uint64{0};

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
                    probe,
                    *candidateXSize,
                    *candidateYSize,
                    best,
                    maximumPixelComparisons,
                    completedPixelComparisons,
                    poll
                );
                completedPixelComparisons = candidate.completedPixelComparisons;

                auto const stop = matchVariant(
                    candidate.outcome,
                    // Ranking stays on the weighted sums because the
                    // normalization factor is one constant for the whole search,
                    // so pruning and the exact-match exit read the same order the
                    // reported scores do while avoiding a division per candidate.
                    [&](uint64 score) -> std::optional<SadSearchStopReason>
                    {
                        if (score < best)
                        {
                            best = score;
                            bestMatch.emplace(
                                candidateX,
                                candidateY,
                                normalizedScore(score, *templatePixels, totalWeight)
                            );
                        }
                        return std::nullopt;
                    },
                    [&](ContrastRefused) -> std::optional<SadSearchStopReason>
                    {
                        ++contrastRefusedCandidates;
                        return std::nullopt;
                    },
                    [](SadSearchStopReason reason) -> std::optional<SadSearchStopReason>
                    {
                        return reason;
                    }
                );
                if (stop)
                {
                    return SadSearchReport{
                        .outcome                   = SadSearchOutcome{*stop},
                        .completedPixelComparisons = completedPixelComparisons,
                        .contrastRefusedCandidates = contrastRefusedCandidates,
                    };
                }
                if (best == 0)
                {
                    return SadSearchReport{
                        .outcome                   = SadSearchOutcome{bestMatch},
                        .completedPixelComparisons = completedPixelComparisons,
                        .contrastRefusedCandidates = contrastRefusedCandidates,
                    };
                }
            }
        }

        return SadSearchReport{
            .outcome                   = SadSearchOutcome{bestMatch},
            .completedPixelComparisons = completedPixelComparisons,
            .contrastRefusedCandidates = contrastRefusedCandidates,
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
