#pragma once

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace uf::workbench
{
    enum class PreviewPageKind : uint8
    {
        Resolved,
        Unknown,
        Ambiguous,
    };

    // The enumerator's name, for a label. Resolved/Unknown/Ambiguous are the
    // author-facing words for the outcome, so they are spelled once here rather
    // than in each panel that shows one.
    [[nodiscard]]
    auto previewPageKindName(PreviewPageKind kind) noexcept -> char const*;

    // One recognizer's evidence as surfaced to the property/preview panels. It
    // flattens the fields the GUI reads from an AnchorEvidence so the panel layer
    // never depends on the recognition module's evidence type.
    struct PreviewAnchorRow final
    {
        annotation::RecognizerId recognizerId;

        bool                     hit{};
        std::optional<uint64>    sadScore{};
        uint64                   maximumSad{};
        std::optional<PixelRect> matchedRect{};
    };

    // A recognizer search that a policy limit interrupted before it produced
    // evidence, naming the recognizer that was running and why it stopped.
    struct PreviewStop final
    {
        annotation::RecognizerId recognizerId;
        SadSearchStopReason      reason{};
    };

    // The outcome of previewing the current document against one source image.
    // The page evaluation always runs: it either classifies the page
    // (m_pageKind, with m_resolvedPageId set only when resolved) or stops
    // (m_pageStop). The action fields are populated only when the selected
    // recognizer is an action target that was evaluated.
    struct PreviewResult final
    {
        // The screen this preview was evaluated against. Its matched rectangles
        // are in that screen's pixel space, so a canvas showing another screen
        // must not draw them; the overlay compares this against the shown source.
        std::optional<annotation::SourceId> sourceId{};

        std::optional<PreviewPageKind>    pageKind{};
        std::optional<annotation::PageId> resolvedPageId{};
        std::vector<PreviewAnchorRow>     anchorRows{};
        std::optional<PreviewStop>        pageStop{};

        std::optional<PreviewAnchorRow> actionEvidence{};
        std::optional<PreviewStop>      actionStop{};

        // Set when a selected action target could not be evaluated on this
        // screen because the element is placed on several pages and this
        // screen's page is not one of them (or the screen is unclaimed). The
        // page and anchor evaluation above still ran; only the action search was
        // skipped, and this states why in a line fit for the status bar. An
        // action evidence is never reported alongside a skip note.
        std::optional<std::string> actionSkipNote{};
    };

    // The pixel-comparison ceiling ONE search may spend. This is the workbench's
    // per-search budget: every preview and model-check search is bounded by this
    // one number, so a single large ROI on a 4K project is measured against it
    // rather than against a whole page's shared remainder. It matches the 256
    // Mi-pixel order of magnitude the authoring compiler bounds its own work with
    // (k_maximumCompilationPixelWork in authoring-compiler.cpp). Hitting it
    // surfaces as a stop reason rather than a miss.
    //
    // The annotation runtime, by contrast, shares one policy.maximumPixelComparisons
    // across every anchor a page evaluation runs (recognition-runtime.cpp), so a
    // page policy must carry this per-search budget scaled by the anchor count;
    // see pagePolicyFor. The runtime's semantics are not changed here: the release
    // cli and engine depend on them, so the scaling lives at the workbench edge.
    inline constexpr auto k_recognitionComparisonBudget = uint64{256} * 1024U * 1024U;

    // Scales a per-search comparison budget to the total a page evaluation needs.
    //
    // evaluatePage shares its policy.maximumPixelComparisons across every anchor
    // it searches, handing each the remainder, so a page of several anchors given
    // only one per-search budget lets the first large search exhaust the whole
    // total and starves the rest. Multiplying the per-search budget by the number
    // of anchor searches restores a full per-search allowance to each. The
    // deadline and cancellation are copied through untouched; only the comparison
    // ceiling scales. A single action-target search keeps the per-search policy
    // unscaled (anchorSearchCount of one). Overflow saturates rather than shrinks:
    // starving searches is the defect this exists to remove.
    [[nodiscard]]
    auto pagePolicyFor(
        annotation::RecognitionPolicy const& perSearchPolicy,
        std::size_t anchorSearchCount
    ) -> annotation::RecognitionPolicy;

    // Compiles the document with its in-memory sources, builds a recognition
    // runtime, and evaluates the page against the selected source's image. When
    // the selected recognizer is an action target, its evidence is evaluated too.
    // The policy carries the comparison budget and any deadline or cancellation,
    // so a caller can preview under a real limit or a zero budget.
    [[nodiscard]]
    auto runPreview(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::SourceId selectedSourceId,
        std::optional<annotation::RecognizerId> selectedRecognizerId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewResult>;

    // One region's score against one captured screen, searched on its own.
    //
    // This answers the question the author has the instant a shared element
    // lands on a page: are these pixels there at all? One template over one
    // region is a fraction of a whole-model check, so it can run where the
    // author is looking rather than behind a button they have to remember.
    //
    // The recognizer supplies both the template and the region to search, so
    // evaluating the element being copied against the screen it is being copied
    // onto gives exactly the score its copy will have -- no copy need exist yet.
    [[nodiscard]]
    auto scoreRegionOnScreen(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::RecognizerId recognizerId,
        annotation::SourceId screenId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewAnchorRow>;

    // What an author sees while picking a colour key: the template rectangle
    // cropped out of its screen with the mask the key implies already written
    // into the alpha channel -- the same bytes the compiler bakes -- and how
    // much of the rectangle survived it.
    //
    // The counts are what make the picker usable. Colour alone is not a matcher:
    // bright white covers 8% of the measured menu region but up to 19% of the
    // artwork beside it, so an author has to see what they selected before
    // trusting it, and a key that keeps 3% of the rectangle has caught a
    // highlight rather than the text.
    //
    // Partially kept pixels are the antialiased rim the tolerance ramp readmits
    // at reduced weight; they are counted apart from the fully kept core because
    // the two answer different questions -- how much is certainly text, and how
    // soft the edge around it is.
    struct ColourKeyMaskPreview final
    {
        uint32 width{};
        uint32 height{};

        std::vector<std::byte> rgbaPixels{};

        std::size_t totalPixels{};
        std::size_t fullyKeptPixels{};
        std::size_t partiallyKeptPixels{};
    };

    // Builds that preview. Without a key the mask is fully opaque, which is
    // exactly what an unkeyed element compiles to.
    [[nodiscard]]
    auto previewColourKeyMask(
        annotation::AuthoringSourceAsset const& asset,
        PixelRect templateRect,
        std::optional<annotation::ColourKey> colourKey
    ) -> Result<ColourKeyMaskPreview>;

    struct SampledSourcePixel final
    {
        uint8 red{};
        uint8 green{};
        uint8 blue{};
    };

    // The colour of one pixel of a captured screen -- the eyedropper. Picking a
    // key is picking a pixel, so this is the whole of that gesture's model.
    [[nodiscard]]
    auto sampleSourcePixel(
        annotation::AuthoringSourceAsset const& asset,
        PixelPoint point
    ) -> Result<SampledSourcePixel>;

    // How one captured screen resolved against the page the document says it
    // stands for.
    enum class ScreenCheckOutcome : uint8
    {
        Correct,
        WrongPage,
        Unknown,
        Ambiguous,
        // No regression case records which page this screen is, so there is
        // nothing to compare the resolution against.
        Unclaimed,
        Stopped,
    };

    struct ScreenCheck final
    {
        annotation::SourceId              sourceId;
        std::optional<annotation::PageId> expectedPageId{};
        std::optional<annotation::PageId> resolvedPageId{};
        ScreenCheckOutcome                outcome{};
    };

    // One recognizer's similarity scores on the screen it was drawn on and on
    // the other screen it comes closest to matching. Both are reported as a raw
    // score against the shared budget rather than a ratio, because maximumSad
    // depends only on the template and the threshold -- never on the screen --
    // so the scores are directly comparable and the comparison stays exact.
    //
    // A well-authored mark scores far below the budget on its own screen and far
    // above it on every other. The gap is the number worth watching: a mark that
    // only passes at nearly zero is over-fitted to a still, and one that nearly
    // passes elsewhere will eventually resolve the wrong page.
    struct RecognizerMargin final
    {
        annotation::RecognizerId recognizerId;
        uint64                   maximumSad{};

        // The screen this recognizer has to work on -- the one recorded for the
        // page it belongs to -- and its score there. For anything drawn on the
        // page it serves that is also the screen its template was cut from. A
        // shared element is the exception: cut from one screen and used on
        // another, it must be read against the page's screen, because that is
        // the only screen it is ever searched on.
        std::optional<annotation::SourceId> ownSourceId{};
        std::optional<uint64>               ownSadScore{};

        std::optional<annotation::SourceId> nearestOtherSourceId{};
        std::optional<uint64>               nearestOtherSadScore{};

        // The score on a frame captured from the running target, when the check
        // was given one. This is the only number that moves on its own: the
        // captured screens are stills that never change, while the live one
        // drifts with highlights, counters, and animation. A mark that passes on
        // its own still and fails here is over-fitted to the still.
        std::optional<uint64> liveSadScore{};
    };

    // How searching one element on one screen came out. Every (element, screen)
    // pair in the grid carries one of these, so a screen a multi-placed element
    // is not placed on is an explicit NotSearchedHere state rather than an empty
    // hole the reader has to interpret.
    enum class ModelCellOutcome : uint8
    {
        // Searched and matched: the score came in at or below the threshold.
        Hit,
        // Searched and did not match: the score stayed above the threshold.
        Miss,
        // The search hit its comparison budget or the run's deadline before it
        // produced evidence, so no score was measured here.
        Stopped,
        // This screen's page does not place the element, so there is no search
        // region for it here -- a multi-placed element off its pages, never an
        // anchor or a single-placement element (those are searched everywhere).
        NotSearchedHere,
    };

    // One (element, screen) observation in the marks-x-screens grid, filed under
    // the ELEMENT id -- never a derived per-page recognizer id -- so the UI's
    // element-keyed lookups reach it the same way the margins do. Derived from
    // the same per-screen evaluation the margins fold in, with no second search.
    struct ModelCheckCell final
    {
        annotation::RecognizerId elementId;
        annotation::SourceId     screenId;
        ModelCellOutcome         outcome{};

        // The measured score and the threshold it was read against, present for
        // Hit and Miss. maximumSad is the same per-element budget the margins
        // report against, so a cell's percentage is comparable across screens.
        std::optional<uint64> sadScore{};
        uint64                maximumSad{};

        // Whether the element is authored to match on this screen: the screen's
        // recorded page places it (an interactive region) or names it (an
        // anchor). This is the ground truth the colour is read against -- a hit
        // where this is false is a misfire, a miss where it is true is a hole.
        // Left false for a cell that was not searched or was stopped.
        bool expectedHit{};

        // Why a Stopped cell stopped -- the budget or the deadline -- for the
        // tooltip. Empty for every other outcome.
        std::optional<SadSearchStopReason> stopReason{};
    };

    // The colour band a cell reads in, one step removed from ImGui so the
    // classification is a pure value the logic-layer tests can pin.
    enum class ModelCellColor : uint8
    {
        // The expected outcome: a hit on a screen the element is authored to
        // match, or a clean miss on one it is not, both with room to spare.
        Expected,
        // A correct-but-close margin, or a search that was stopped: worth a look
        // even though nothing is yet wrong.
        Thin,
        // A wrong outcome: a hit on a foreign screen, or a miss on a screen the
        // element's own page places it.
        Misfire,
        // Not searched here: dim, an explicit absence rather than a verdict.
        NotSearched,
    };

    // The thin-margin band, as a fraction (numerator / denominator) of the
    // threshold. A measured score whose distance from its threshold is within
    // this fraction reads amber, because a mark that only just passes -- or only
    // just fails -- is a frame of drift from flipping. The margin view carries no
    // "thin" definition to inherit, so ten percent is the documented choice.
    inline constexpr auto k_thinMarginNumerator   = uint64{10};
    inline constexpr auto k_thinMarginDenominator = uint64{100};

    // The colour a cell reads in, from its outcome, its measured margin, and
    // whether the element is authored to match on its screen. Pure and total: it
    // reads only the cell, so the tests hold the whole classification without a
    // document or a GUI. Precedence is wrong-outcome (red) over stopped-or-thin
    // (amber) over clear-and-correct (green), so a misfire never hides behind a
    // thin band.
    [[nodiscard]]
    auto classifyModelCell(ModelCheckCell const& cell) noexcept -> ModelCellColor;

    // The running target's current screen. A live frame carries no recorded
    // expectation -- the author is looking at it -- so only how the page
    // classified is reported, with no correct-or-not verdict attached.
    struct LiveScreenCheck final
    {
        std::optional<PreviewPageKind>    pageKind{};
        std::optional<annotation::PageId> resolvedPageId{};
        std::optional<PreviewStop>        stop{};
    };

    struct ModelCheck final
    {
        std::vector<ScreenCheck>       screens{};
        std::vector<RecognizerMargin>  margins{};
        std::optional<LiveScreenCheck> live{};

        // One cell per (element, screen) over the captured screens, filed under
        // the element id. The full grid the margins fold away: the margins keep
        // each mark's own score and nearest-other margin, while these preserve
        // every per-screen outcome the run measured. The live frame contributes
        // no cell -- it is not a project screen and has no column.
        std::vector<ModelCheckCell> cells{};
    };

    // Evaluates every recognizer against every captured screen, once, and reports
    // both what each screen resolved to and how much room each recognizer has.
    // This is the check an author cannot perform by eye: a mark always matches
    // the image it was cut from at a score of zero, so the only evidence that it
    // identifies one screen rather than another is how it scores on the others.
    //
    // liveFrameBytes is a PNG freshly captured from the running target, or empty
    // when there is none. It is evaluated as one more screen and folded into the
    // same margins, so the author reads one row per recognizer rather than
    // comparing two reports. It is deliberately not added to the project: a frame
    // taken to measure against is not a screen the model is authored on.
    [[nodiscard]]
    auto runModelCheck(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        std::span<std::byte const> liveFrameBytes,
        annotation::RecognitionPolicy const& policy
    ) -> Result<ModelCheck>;
}
