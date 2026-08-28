#pragma once

#include "template-match.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/frame.hpp>
#include <domain/space.hpp>

#include <span>
#include <string>
#include <vector>

namespace uf
{
    // Opaque Gray8 samples, already scaled and rotated by the caller. NCC has
    // no colour gate and no alpha weighting; a nonempty mask is refused.
    struct ShapeTemplate final
    {
        GrayTemplateImage image{};
        double            minimumScore{};
    };

    struct ShapeSearchOptions final
    {
        uint32 maximumMatches{512};
        uint32 suppressionRadius{10};
    };

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-member-init): PixelRect has no default state.
    struct ShapeMatch final
    {
        std::string templateIdentity{};
        PixelRect   matchedRect;
        double      score{};
    };

    struct ShapeSearchReport final
    {
        std::vector<ShapeMatch> matches{};

        // Counts template/pixel products actually evaluated. ROI conversion and
        // integral moments do not compare templates and spend no comparisons.
        uint64 completedPixelComparisons{};
        uint32 imageWidth{};
        uint32 imageHeight{};
    };

    // Exact zero-mean normalized cross correlation over every valid position.
    // Constant patches do not match; constant templates are invalid. Each
    // minimumScore threshold must lie in [0, 1]; raw NCC scores lie in [-1, 1].
    // Positive affine brightness changes preserve a score
    // unless clipping or Gray8 quantization changes the shape.
    //
    // At most 64 templates, each at most 64 by 64 pixels, and an ROI of at most
    // 4,194,304 pixels are accepted. Candidates above threshold are bounded at
    // 65,536 before global suppression. Every exceeded bound, including the
    // requested 1..512 output limit, is a refusal, never a partial answer.
    // Suppression uses integer rectangle centres and a square radius in 0..64.
    // Results sort by decreasing score, then template identity, y, x.
    [[nodiscard]]
    auto matchShapesOnFrame(
        Frame const& frame,
        std::span<ShapeTemplate const> templates,
        PixelRect searchRoi,
        RecognitionPolicy const& policy,
        ShapeSearchOptions const& options
    ) -> Result<ShapeSearchReport>;
}
