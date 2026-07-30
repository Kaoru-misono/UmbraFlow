#pragma once

#include "bgra-image.hpp"
#include "sad.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <cstddef>
#include <span>
#include <variant>
#include <vector>

// Three questions about a set of frames of one screen, each answered by hand
// once and each too slow to answer that way twice.
//
// Frames arrive already decoded, as BGRA8 planes; this module never touches a
// file. Every function takes the frames as one span rather than a pair, because
// the useful set is two backgrounds plus a frame taken seconds later to catch
// animation, and a pixel that is stable in the first two and moves in the third
// is not stable.
//
// The scanning primitives share the matcher's budget and cancellation
// vocabulary -- SadSearchPoll, SadSearchControl, SadSearchStopReason and
// k_sadSearchPollIntervalComparisons from sad.hpp -- rather than declaring a
// parallel set. The Sad prefix on those names is historical; one convention per
// module is worth more than a better prefix.
namespace uf
{
    // The mask weight one pixel earns from a colour key: full weight out to the
    // tolerance, then a linear ramp to nothing at twice it, with a tolerance of
    // zero staying an exact-colour match. Distance is the sum of the three
    // channel distances, so 765 is the widest tolerance that means anything.
    //
    // This is the rule's one implementation. annotation::ColourKey::alphaFor
    // delegates here and documents why the ramp exists; the dependency runs
    // annotation -> vision, so the call can only go that way.
    //
    // It lives in vision rather than annotation because probeColour needs it to
    // answer how many pixels a key selects, which is the question that whole
    // function exists for. Taking the weights as a caller-supplied plane was the
    // alternative and would have left this module unable to answer it.
    [[nodiscard]]
    auto colourKeyAlpha(
        Bgra8Pixel pixel,
        uint8 keyRed,
        uint8 keyGreen,
        uint8 keyBlue,
        uint32 tolerance
    ) noexcept -> uint8;

    // The widest colour-key tolerance that means anything, matching
    // annotation::ColourKey.
    inline constexpr auto k_maximumColourKeyTolerance = uint32{765};

    // A rectangle of pixels that held still across every analysed frame, in the
    // coordinates of the frames rather than of the analysed rect.
    struct StableRegion final
    {
        // No in-class initializer: PixelRect has no default state, so every
        // construction site supplies the bounds.
        PixelRect bounds;

        // Stable pixels inside those bounds. A region is a bounding box, not a
        // solid block, so this is what tells a caller whether it found a glyph
        // or a scatter of coincidences.
        uint64 stablePixels{};

        auto operator==(StableRegion const&) const -> bool = default;
    };

    struct StabilitySpec final
    {
        // The rectangle analysed inside every frame. Every frame must be large
        // enough to contain it; frames need not share an extent otherwise.
        PixelRect rect;

        // Frames agree at a pixel when the largest grey value any of them gave
        // it exceeds the smallest by at most this. Zero demands byte identity,
        // which is what UI drawn over changing artwork actually produces.
        uint32 grayTolerance{};

        // A run of this many consecutive fully unstable rows, or columns,
        // splits a region in two. Zero disables splitting and reports the one
        // bounding box over every stable pixel -- the naive answer, which
        // merges a menu label with the sub-label under it.
        uint32 minimumGap{};
    };

    struct StabilityReport final
    {
        // 255 where every frame agreed and 0 elsewhere, row major and tightly
        // packed over rect.width() by rect.height(). Shaped so it can be handed
        // straight to matchTemplateSad as a template mask.
        std::vector<std::byte> stableMask{};

        // Stable pixels per row and per column of the rect, in rect-relative
        // order. These are the projections the regions are cut from; a caller
        // that wants a different cut, or wants to see where the gaps are before
        // choosing minimumGap, reads them instead of re-deriving them.
        std::vector<uint32> rowProfile{};
        std::vector<uint32> columnProfile{};

        // Ordered top to bottom, then left to right. Empty when no pixel was
        // stable.
        std::vector<StableRegion> regions{};

        uint64 stablePixels{};
        uint64 rectPixels{};

        // Mean over the rect of each pixel's grey spread: the largest grey
        // value any frame gave it minus the smallest. This is the whole-rect
        // number a masked probe has to be read against.
        double meanGraySpread{};
    };

    using StabilityOutcome = std::variant<
        StabilityReport,
        SadSearchStopReason
    >;

    struct StabilityScan final
    {
        StabilityOutcome outcome{};

        // Counts pixel reads actually executed, one per pixel per frame. Valid
        // for every outcome and starts at zero for each call.
        uint64 completedPixelVisits{};
    };

    // Reports which pixels of spec.rect held still across every frame. At least
    // two frames are required: one frame is stable everywhere and answers
    // nothing.
    [[nodiscard]]
    auto analyseStability(
        std::span<BgraImage const> frames,
        StabilitySpec const& spec
    ) -> Result<StabilityReport>;

    [[nodiscard]]
    auto analyseStability(
        std::span<BgraImage const> frames,
        StabilitySpec const& spec,
        uint64 maximumPixelVisits,
        SadSearchPoll const& poll
    ) -> Result<StabilityScan>;

    struct ColourProbeSpec final
    {
        // No in-class initializer: PixelRect has no default state.
        PixelRect rect;

        uint8 keyRed{};
        uint8 keyGreen{};
        uint8 keyBlue{};

        // Rejected above k_maximumColourKeyTolerance.
        uint32 tolerance{};
    };

    struct ColourProbeReport final
    {
        uint64 rectPixels{};

        // What the key takes at full weight, and what it takes at a partial
        // weight on the ramp. Selection reads the first frame, which is the
        // frame an author picked the colour from.
        uint64 fullySelectedPixels{};
        uint64 rampSelectedPixels{};

        // Sum of the weights the key handed out over the rect, which is what
        // the masked mean is divided by. Zero when the key selects nothing.
        uint64 selectedWeight{};

        // Mean grey spread across frames over the selected pixels, each pixel
        // weighted by the weight it earned. Zero when nothing is selected.
        double maskedMeanGraySpread{};

        // The same mean over every pixel of the rect, weight ignored. The gap
        // between the two is the whole answer to whether this rect survives a
        // background change.
        double rectMeanGraySpread{};
    };

    using ColourProbeOutcome = std::variant<
        ColourProbeReport,
        SadSearchStopReason
    >;

    struct ColourProbeScan final
    {
        ColourProbeOutcome outcome{};
        uint64             completedPixelVisits{};
    };

    // Measures how well a colour key isolates whatever holds still in a rect.
    // At least two frames are required, for the same reason analyseStability
    // requires them.
    [[nodiscard]]
    auto probeColour(
        std::span<BgraImage const> frames,
        ColourProbeSpec const& spec
    ) -> Result<ColourProbeReport>;

    [[nodiscard]]
    auto probeColour(
        std::span<BgraImage const> frames,
        ColourProbeSpec const& spec,
        uint64 maximumPixelVisits,
        SadSearchPoll const& poll
    ) -> Result<ColourProbeScan>;

    struct ColourCount final
    {
        uint8 blue{};
        uint8 green{};
        uint8 red{};

        uint64 count{};

        auto operator==(ColourCount const&) const -> bool = default;
    };

    struct ColourCensusSpec final
    {
        // No in-class initializer: PixelRect has no default state.
        PixelRect rect;

        // How many of the most frequent colours to report. Zero reports none
        // and still answers how many distinct colours the rect holds.
        uint32 maximumEntries{};
    };

    struct ColourCensusReport final
    {
        uint64 rectPixels{};
        uint64 distinctColours{};

        // Most frequent first. Equal counts are ordered by packed blue, green,
        // red value ascending, so the report never depends on traversal order.
        std::vector<ColourCount> dominant{};
    };

    // Counts the colours of one frame's rect, so a caller picks a key from data
    // instead of sampling a pixel and hoping. Alpha is ignored: a captured
    // frame's alpha carries no colour. Single frame and O(pixels), so unlike
    // the scans above it takes no budget.
    [[nodiscard]]
    auto censusColours(
        BgraImage const& frame,
        ColourCensusSpec const& spec
    ) -> Result<ColourCensusReport>;
}
