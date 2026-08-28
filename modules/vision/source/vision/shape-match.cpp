#include "shape-match.hpp"
#include "bgra-image.hpp"

#include <core/error/contracts.hpp>
#include <core/error/result.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ranges>
#include <span>
#include <string>
#include <vector>

namespace uf
{
    namespace
    {
        constexpr auto k_maximumRoiPixels = uint64{4'194'304};
        constexpr auto k_maximumCandidates = std::size_t{65'536};

        [[nodiscard]]
        auto pollSearch(SadSearchPoll const& poll) -> Status
        {
            switch (poll())
            {
            case SadSearchControl::Continue: return ok();
            case SadSearchControl::Cancelled:
                return fail(AutomationErrorKind::Cancelled, "shape search cancelled");
            case SadSearchControl::TimedOut:
                return fail(AutomationErrorKind::Timeout, "shape search timed out");
            }
            UF_UNREACHABLE_MSG("Unknown shape search control");
        }

        struct TemplateMoments final
        {
            uint64 sum{};
            double variance{};
        };

        [[nodiscard]]
        auto validateTemplates(std::span<ShapeTemplate const> templates)
            -> Result<std::vector<TemplateMoments>>
        {
            if (templates.empty() || templates.size() > 64)
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "shape search requires between 1 and 64 templates"
                );
            }
            auto moments = std::vector<TemplateMoments>{};
            moments.reserve(templates.size());
            for (auto index = std::size_t{0}; index < templates.size(); ++index)
            {
                auto const& shape = templates[index];
                auto const& image = shape.image;
                auto const pixels = uint64{image.width} * image.height;
                if (
                    image.identity.empty() || image.identity.size() > 128
                    || image.width == 0 || image.width > 64
                    || image.height == 0 || image.height > 64
                    || image.pixels.size() != pixels || !image.mask.empty()
                    || !std::isfinite(shape.minimumScore)
                    || shape.minimumScore < 0 || shape.minimumScore > 1
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "invalid opaque shape template geometry, identity or score"
                    );
                }
                for (auto earlier = std::size_t{0}; earlier < index; ++earlier)
                {
                    if (templates[earlier].image.identity == image.identity)
                    {
                        return fail(
                            AutomationErrorKind::InvalidResource,
                            "shape template identities must be unique"
                        );
                    }
                }
                auto sum     = uint64{0};
                auto squares = uint64{0};
                for (auto byte : image.pixels)
                {
                    auto const value = std::to_integer<uint64>(byte);
                    sum += value;
                    squares += value * value;
                }
                // A 64-square Gray8 plane bounds each product below 2^40, so
                // these integer moments are exact before conversion to double.
                auto const variance = static_cast<double>(pixels * squares - sum * sum);
                if (variance == 0)
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "constant shape template has no normalized correlation"
                    );
                }
                moments.emplace_back(TemplateMoments{.sum = sum, .variance = variance});
            }
            return moments;
        }

        class RoiMoments final
        {
            std::size_t              m_width;
            std::size_t              m_height;
            std::vector<std::byte>   m_pixels;
            std::vector<uint64>      m_sum;
            std::vector<uint64>      m_squares;

            explicit RoiMoments(std::size_t width, std::size_t height)
                : m_width{width}
                , m_height{height}
                , m_pixels(width * height)
                , m_sum((width + 1) * (height + 1))
                , m_squares((width + 1) * (height + 1))
            {
            }

            [[nodiscard]]
            auto rectangleSum(
                std::span<uint64 const> table,
                std::size_t x,
                std::size_t y,
                std::size_t width,
                std::size_t height
            ) const noexcept -> uint64
            {
                auto const stride = m_width + 1;
                auto const top = y * stride;
                auto const bottom = (y + height) * stride;
                return (
                    checkedAt(table, bottom + x + width) - checkedAt(table, top + x + width)
                    - checkedAt(table, bottom + x) + checkedAt(table, top + x)
                );
            }

        public:
            [[nodiscard]]
            static auto fromFrame(
                Frame const& frame,
                PixelRect roi,
                SadSearchPoll const& poll
            ) -> Result<RoiMoments>
            {
                auto const owner = frame.pixels();
                UF_CHECK(owner != nullptr);
                switch (frame.pixelFormat())
                {
                case PixelFormat::Gray8:
                {
                    UF_TRY_VALUE(
                        gray, GrayImage::create(owner->bytes(), frame.width(), frame.height(), frame.stride())
                    );
                    return create(gray, roi, poll);
                }
                case PixelFormat::Bgra8:
                {
                    UF_TRY_VALUE(
                        bgra, BgraImage::create(owner->bytes(), frame.width(), frame.height(), frame.stride())
                    );
                    // Convert only the requested region; neither matching nor
                    // grayscale preprocessing reads pixels outside its bounds.
                    auto pixels = std::vector<std::byte>{};
                    pixels.reserve(std::size_t{roi.width()} * roi.height());
                    for (auto y = roi.y(); y < roi.bottom(); ++y)
                    {
                        for (auto x = roi.x(); x < roi.right(); ++x)
                        {
                            if ((x - roi.x()) % 4096 == 0)
                            {
                                UF_TRY(pollSearch(poll));
                            }
                            pixels.emplace_back(std::byte{bgra.grayAt(x, y)});
                        }
                    }
                    UF_TRY_VALUE(
                        gray, GrayImage::create(pixels, roi.width(), roi.height(), roi.width())
                    );
                    UF_TRY_VALUE(local, PixelRect::create(0, 0, roi.width(), roi.height()));
                    return create(gray, local, poll);
                }
                }
                UF_UNREACHABLE_MSG("Unknown shape search pixel format");
            }

            [[nodiscard]]
            static auto create(
                GrayImage const& image,
                PixelRect roi,
                SadSearchPoll const& poll
            ) -> Result<RoiMoments>
            {
                // The caller checked the ROI against the frame and the four
                // million pixel cap. Border cells fit size_t even on 32-bit.
                auto result = RoiMoments{roi.width(), roi.height()};
                for (auto y = std::size_t{0}; y < result.m_height; ++y)
                {
                    UF_TRY(pollSearch(poll));
                    auto const rowIndex = checkedCast<uint32>(y + roi.y());
                    UF_CHECK(rowIndex.has_value());
                    auto const row = image.row(*rowIndex);
                    UF_CHECK(row.has_value());
                    auto const source = row->subspan(roi.x(), roi.width());
                    auto runningSum     = uint64{0};
                    auto runningSquares = uint64{0};
                    for (auto x = std::size_t{0}; x < result.m_width; ++x)
                    {
                        if (x % 4096 == 0)
                        {
                            UF_TRY(pollSearch(poll));
                        }
                        auto const value = std::to_integer<uint64>(source[x]);
                        runningSum += value;
                        runningSquares += value * value;
                        auto const above = y * (result.m_width + 1) + x + 1;
                        auto const cell = above + result.m_width + 1;
                        result.m_sum[cell] = result.m_sum[above] + runningSum;
                        result.m_squares[cell] = result.m_squares[above] + runningSquares;
                        result.m_pixels[y * result.m_width + x] = source[x];
                    }
                }
                return result;
            }

            [[nodiscard]]
            auto moments(std::size_t x, std::size_t y, GrayTemplateImage const& image)
                const noexcept -> TemplateMoments
            {
                auto const pixels = uint64{image.width} * image.height;
                auto const sum = rectangleSum(m_sum, x, y, image.width, image.height);
                auto const squares = rectangleSum(m_squares, x, y, image.width, image.height);
                return TemplateMoments{
                    .sum      = sum,
                    .variance = static_cast<double>(pixels * squares - sum * sum),
                };
            }

            [[nodiscard]]
            auto dotProduct(std::size_t x, std::size_t y, GrayTemplateImage const& image)
                const noexcept -> uint64
            {
                auto sum = uint32{0};
                auto const plane = std::span<std::byte const>{m_pixels};
                auto const pattern = std::span<std::byte const>{image.pixels};
                for (auto row = std::size_t{0}; row < image.height; ++row)
                {
                    auto const left = plane.subspan((y + row) * m_width + x, image.width);
                    auto const right = pattern.subspan(row * image.width, image.width);
                    // Both spans have the same validated width. The total is
                    // at most 4096 * 255^2, which fits uint32 and vectorizes.
                    for (auto column = std::size_t{0}; column < left.size(); ++column)
                    {
                        sum += (
                            std::to_integer<uint32>(left[column])
                            * std::to_integer<uint32>(right[column])
                        );
                    }
                }
                return sum;
            }
        };

        struct Candidate final
        {
            std::size_t templateIndex{};
            uint32      x{};
            uint32      y{};
            double      score{};
        };

        [[nodiscard]]
        auto searchShapes(
            RoiMoments const& plane,
            std::span<ShapeTemplate const> templates,
            std::span<TemplateMoments const> moments,
            PixelRect roi,
            RecognitionPolicy const& policy,
            ShapeSearchOptions const& options,
            SadSearchPoll const& poll
        ) -> Result<ShapeSearchReport>
        {
            auto candidates  = std::vector<Candidate>{};
            auto comparisons = uint64{0};
            auto nextPoll    = uint64{0};
            for (auto index = std::size_t{0}; index < templates.size(); ++index)
            {
                auto const& shape = templates[index];
                auto const& image = shape.image;
                auto const pixels = uint64{image.width} * image.height;
                auto const templateMoments = checkedAt(moments, index);
                for (auto y = uint32{0}; y <= roi.height() - image.height; ++y)
                {
                    UF_TRY(pollSearch(poll));
                    for (auto x = uint32{0}; x <= roi.width() - image.width; ++x)
                    {
                        if (x % 256 == 0)
                        {
                            UF_TRY(pollSearch(poll));
                        }
                        auto const patch = plane.moments(x, y, image);
                        if (patch.variance == 0)
                        {
                            continue;
                        }
                        if (pixels > policy.maximumPixelComparisons - comparisons)
                        {
                            return fail(
                                AutomationErrorKind::RecognitionIncomplete,
                                "shape search comparison budget exhausted"
                            );
                        }
                        if (comparisons >= nextPoll)
                        {
                            UF_TRY(pollSearch(poll));
                            nextPoll = comparisons + k_sadSearchPollIntervalComparisons;
                        }
                        auto const dot = plane.dotProduct(x, y, image);
                        comparisons += pixels;
                        auto const covariance = (
                            static_cast<double>(pixels * dot)
                            - static_cast<double>(patch.sum * templateMoments.sum)
                        );
                        auto const score = std::clamp(
                            covariance / std::sqrt(patch.variance * templateMoments.variance),
                            -1.0, 1.0
                        );
                        if (score < shape.minimumScore)
                        {
                            continue;
                        }
                        if (candidates.size() == k_maximumCandidates)
                        {
                            return fail(
                                AutomationErrorKind::RecognitionIncomplete,
                                "shape search candidate bound exceeded"
                            );
                        }
                        candidates.emplace_back(
                            Candidate{.templateIndex = index, .x = x, .y = y, .score = score}
                        );
                    }
                }
            }
            UF_TRY(pollSearch(poll));
            std::ranges::sort(
                candidates,
                [templates](Candidate const& left, Candidate const& right)
                {
                    if (left.score != right.score)
                    {
                        return left.score > right.score;
                    }
                    auto const& leftId = checkedAt(templates, left.templateIndex).image.identity;
                    auto const& rightId = checkedAt(templates, right.templateIndex).image.identity;
                    if (leftId != rightId)
                    {
                        return leftId < rightId;
                    }
                    return left.y != right.y ? left.y < right.y : left.x < right.x;
                }
            );
            auto report = ShapeSearchReport{
                .completedPixelComparisons = comparisons,
            };
            for (auto const& candidate : candidates)
            {
                UF_TRY(pollSearch(poll));
                auto const& image = checkedAt(templates, candidate.templateIndex).image;
                auto const x = roi.x() + candidate.x;
                auto const y = roi.y() + candidate.y;
                auto const centerX = int64{x} + image.width / 2;
                auto const centerY = int64{y} + image.height / 2;
                auto const suppressed = std::ranges::any_of(
                    report.matches,
                    [centerX, centerY, radius = options.suppressionRadius](ShapeMatch const& match)
                    {
                        auto const& rect = match.matchedRect;
                        return (
                            std::abs(centerX - int64{rect.x()} - rect.width() / 2) <= radius
                            && std::abs(centerY - int64{rect.y()} - rect.height() / 2) <= radius
                        );
                    }
                );
                if (suppressed)
                {
                    continue;
                }
                if (report.matches.size() == options.maximumMatches)
                {
                    return fail(
                        AutomationErrorKind::RecognitionIncomplete,
                        "shape search match bound exceeded"
                    );
                }
                UF_TRY_VALUE(rect, PixelRect::create(x, y, image.width, image.height));
                report.matches.emplace_back(
                    ShapeMatch{
                        .templateIdentity = image.identity,
                        .matchedRect      = rect,
                        .score            = candidate.score,
                    }
                );
            }
            UF_TRY(pollSearch(poll));
            return report;
        }
    }

    auto matchShapesOnFrame(
        Frame const& frame,
        std::span<ShapeTemplate const> templates,
        PixelRect searchRoi,
        RecognitionPolicy const& policy,
        ShapeSearchOptions const& options
    ) -> Result<ShapeSearchReport>
    {
        UF_TRY(searchRoi.ensureWithinExtent(frame.width(), frame.height()));
        if (
            options.maximumMatches == 0 || options.maximumMatches > 512
            || options.suppressionRadius > 64
            || uint64{searchRoi.width()} * searchRoi.height() > k_maximumRoiPixels
        )
        {
            return fail(AutomationErrorKind::InvalidResource, "invalid shape search bounds");
        }
        auto const poll = makeSadSearchPoll(policy);
        UF_TRY(pollSearch(poll));
        UF_TRY_VALUE(moments, validateTemplates(templates));
        for (auto const& shape : templates)
        {
            if (shape.image.width > searchRoi.width() || shape.image.height > searchRoi.height())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "shape template does not fit the search ROI"
                );
            }
        }
        UF_TRY_VALUE(plane, RoiMoments::fromFrame(frame, searchRoi, poll));
        UF_TRY_VALUE(
            report, searchShapes(plane, templates, moments, searchRoi, policy, options, poll)
        );
        report.imageWidth  = frame.width();
        report.imageHeight = frame.height();
        return report;
    }
}
