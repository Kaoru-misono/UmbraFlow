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

    // One element's evidence as surfaced to the property/preview panels. It
    // flattens the fields the GUI reads from an AnchorEvidence so the panel layer
    // never depends on the recognition module's evidence type.
    struct PreviewAnchorRow final
    {
        annotation::ElementId elementId;

        // Which appearance produced this evidence. Absent only for an element
        // that declares none and is located by the page being recognised.
        std::optional<annotation::ResourceName> appearance{};

        bool                     hit{};
        std::optional<uint64>    sadScore{};
        uint64                   maximumSad{};
        std::optional<PixelRect> matchedRect{};
    };

    // An element search that a policy limit interrupted before it produced
    // evidence, naming the element that was running and why it stopped.
    struct PreviewStop final
    {
        annotation::ElementId elementId;
        SadSearchStopReason   reason{};
    };

    // The outcome of previewing the current document against one source image.
    // The page evaluation always runs: it either classifies the page
    // (m_pageKind, with m_resolvedPageId set only when resolved) or stops
    // (m_pageStop). The action fields are populated only when the selected
    // element is an action target that was evaluated.
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
    // the selected element is an action target, its evidence is evaluated too.
    // The policy carries the comparison budget and any deadline or cancellation,
    // so a caller can preview under a real limit or a zero budget.
    [[nodiscard]]
    auto runPreview(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::SourceId selectedSourceId,
        std::optional<annotation::ElementId> selectedElementId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewResult>;

    // One region's score against one captured screen, searched on its own.
    //
    // This answers the question the author has the instant a shared element
    // lands on a page: are these pixels there at all? One template over one
    // region is a fraction of a whole-model check, so it can run where the
    // author is looking rather than behind a button they have to remember.
    //
    // The element supplies the appearance and its reference the region, so
    // evaluating the element against the screen it is being put onto gives
    // exactly the score the new reference will have -- it need not exist yet.
    // Fails for an element no page exercises interact on: with no reference
    // there is no region to search.
    [[nodiscard]]
    auto scoreRegionOnScreen(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::ElementId elementId,
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

    // One element's similarity scores on the screen it was drawn on and on
    // the other screen it comes closest to matching. Both are reported as a raw
    // score against the shared budget rather than a ratio, because maximumSad
    // depends only on the template and the threshold -- never on the screen --
    // so the scores are directly comparable and the comparison stays exact.
    //
    // A well-authored mark scores far below the budget on its own screen and far
    // above it on every other. The gap is the number worth watching: a mark that
    // only passes at nearly zero is over-fitted to a still, and one that nearly
    // passes elsewhere will eventually resolve the wrong page.
    struct ElementMargin final
    {
        annotation::ElementId elementId;
        uint64                maximumSad{};

        // The screen this element has to work on -- the one recorded for the
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
    // pair in the grid carries one of these, so a pair nothing measured is an
    // explicit NotSearchedHere state rather than an empty hole the reader has to
    // interpret.
    enum class ModelCellOutcome : uint8
    {
        // Searched and matched: the score came in at or below the threshold.
        Hit,
        // Searched and did not match: the score stayed above the threshold.
        Miss,
        // The search hit its comparison budget or the run's deadline before it
        // produced evidence, so no score was measured here.
        Stopped,
        // Nothing searched for the element here. One element compiles to one
        // compiled element, so an element some page exercises is searched on every
        // screen, on and off its own pages -- which is what makes the grid's
        // off-diagonal cells measurable. What is left is an element no page
        // exercises at all: it has no region to be searched in anywhere.
        NotSearchedHere,
    };

    // What one row of the grid searched.
    //
    // An element with several appearances collapses, once folded, into a single
    // Hit or Miss per screen -- and that single answer cannot say whether each
    // appearance discriminates or whether one of them matches everywhere and
    // carries the rest. Both questions have to be asked, so the grid holds both
    // rows and the property each one supports is different.
    enum class ModelCellSubject : uint8
    {
        // Every declared appearance searched and folded into one answer. This
        // is what the runtime does, and therefore what a page signature and a
        // click actually read.
        Element,
        // One declared appearance searched alone, which no runtime path does.
        // It exists to ask whether that appearance discriminates by itself,
        // including on the screens another appearance owns -- where a hit is
        // invisible once folded with a correct one.
        Appearance,
    };

    // What the model declares about one row's subject on one screen -- the
    // ground truth a measured outcome is read against.
    //
    // Three answers, because the model can make three statements, and the third
    // is the one this grid used to be unable to make. A page's signature is a
    // conjunction over the marks that identify it, never an inventory of what
    // is on its screen, so "the page recorded for this screen does not name this
    // element" is a different sentence from "these pixels are not there". An
    // overlay proves it: a card-detail screen is the battle screen with a card
    // selected, and the draw pile, discard pile and end-turn button are all
    // still on it, merely dimmed. Reading each of those as a misfire trains an
    // author to break a model that is correct.
    enum class ModelCellExpectation : uint8
    {
        // The model states this subject matches here.
        Match,
        // The model states this subject does not match here.
        Absent,
        // The model states nothing here. Neither outcome is a defect, and the
        // measured score is reported for its own sake.
        Unclaimed,
    };

    // Whether a hit is what the model asks for. The one place the tri-state
    // collapses back to the boolean the JSON surface reports.
    [[nodiscard]]
    auto expectsHit(ModelCellExpectation expectation) noexcept -> bool;

    // One (element, appearance, screen) observation in the falsification grid,
    // filed under the element id, the one name a signature, an authorisation, a
    // trace line, and the UI's own lookups all resolve. Derived from the same
    // per-screen evaluation the margins fold in, with no second search of the
    // folded element.
    //
    // An element declaring one appearance gets no Appearance rows: the folded
    // row IS that appearance's row, and searching it again would double the
    // cost of every project authored so far to restate a measurement already
    // present.
    struct ModelCheckCell final
    {
        annotation::ElementId elementId;
        annotation::SourceId  screenId;
        ModelCellSubject      subject{};

        // Which appearance this row's evidence names: the one searched on an
        // Appearance row, the one the fold settled on an Element row. Absent
        // for an element that declares none and is located by its page, and for
        // a row nothing measured.
        std::optional<annotation::ResourceName> appearance{};

        ModelCellOutcome outcome{};

        // The measured score and the threshold it was read against, present for
        // Hit and Miss. maximumSad belongs to the appearance that produced the
        // score, never to the element, so two rows of one element are comparable
        // only through the cross product judgeModelCheck uses.
        std::optional<uint64> sadScore{};
        uint64                maximumSad{};

        // Where the search landed. On an Element row this is the fold's verdict,
        // and it is the rectangle resolveClickPixel derives the click from -- so
        // a fold that answers with the right appearance and the wrong rectangle
        // has still moved the click.
        std::optional<PixelRect> matchedRect{};

        // What the model states about this row's subject on this screen.
        //
        // For an Element row it is the statement the page recorded for the
        // screen makes about the element -- required, forbidden, or clicked and
        // read, which also puts it on that screen -- and failing that, the duty
        // another page's signature leaves resting on it. For an Appearance row
        // it is that same statement narrowed to the appearance the model names
        // for this screen. A hit against Absent is a misfire, a miss against
        // Match is a hole, and against Unclaimed neither is wrong.
        ModelCellExpectation expectation{ModelCellExpectation::Unclaimed};

        // Why a Stopped cell stopped -- the budget or the deadline -- for the
        // tooltip. Empty for every other outcome.
        std::optional<SadSearchStopReason> stopReason{};
    };

    // The colour band a cell reads in, one step removed from ImGui so the
    // classification is a pure value the logic-layer tests can pin.
    enum class ModelCellColor : uint8
    {
        // Nothing to answer for: the outcome the model asks for, with room to
        // spare, or an outcome the model asks nothing about.
        Expected,
        // A correct-but-close margin, or a search that was stopped: worth a look
        // even though nothing is yet wrong.
        Thin,
        // A wrong outcome: a hit where the model states the subject is absent,
        // or a miss where it states the subject matches.
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
    // what the model states about its subject on its screen. Pure and total: it
    // reads only the cell, so the tests hold the whole classification without a
    // document or a GUI. Precedence is wrong-outcome (red) over stopped-or-thin
    // (amber) over clear-and-correct (green), so a misfire never hides behind a
    // thin band. A cell the model states nothing about is never red in either
    // direction; its margin still reads amber, because a score a frame from
    // flipping is worth seeing whether or not the flip would be a defect.
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
        std::vector<ElementMargin>     margins{};
        std::optional<LiveScreenCheck> live{};

        // One cell per (element, screen), plus one per (element, appearance,
        // screen) for every element declaring more than one appearance, over the
        // captured screens. The full grid the margins fold away: the margins
        // keep each mark's own score and nearest-other margin, while these
        // preserve every per-screen outcome the run measured. The live frame
        // contributes no cell -- it is not a project screen and has no column.
        std::vector<ModelCheckCell> cells{};
    };

    // How far the appearance a screen belongs to must beat every other
    // appearance of the same element there.
    //
    // Two appearances of one element are not comparable by raw score --
    // maximumSad is a function of each one's own template size and threshold --
    // so the factor is applied to the exact integer cross product and never to a
    // ratio. A margin narrower than this is a model that happens to be right on
    // the stills it was authored against: the repository's own measured
    // cross-screen misses sat at 2.85x to 4.15x the threshold
    // (docs/pitfalls/page-modeling-and-multi-step.md), so four is inside what a
    // healthy mark has already been observed to deliver and outside what drift
    // can close in a frame. It is deliberately a constant rather than a flag:
    // tuned per project it would slide to one, and at one this check degenerates
    // into "the right appearance won", which P3 already asserts.
    inline constexpr auto k_appearanceSeparationFactor = uint64{4};

    // What one falsification failure is. Three kinds, because three things can
    // be wrong and each needs a different repair.
    enum class ModelFindingKind : uint8
    {
        // A measured outcome contradicting the model: a match where the row's
        // subject does not belong, or none where it does. An appearance that
        // matches a screen another appearance owns lands here, and it is the
        // failure the appearance rows exist to expose.
        WrongOutcome,
        // The element matched a screen one of its appearances owns, but the fold
        // answered with a different appearance -- or with the right one and a
        // rectangle that is not where that appearance matched. Either way the
        // click moves, and nothing downstream can tell.
        WrongAppearance,
        // The owning appearance beat a rival by less than
        // k_appearanceSeparationFactor, so which one answers is a frame of drift
        // away from changing. Both outcomes are still correct here; that is what
        // makes it worth reporting rather than waiting for.
        ThinSeparation,
    };

    struct ModelFinding final
    {
        annotation::ElementId elementId;
        annotation::SourceId  screenId;
        ModelFindingKind      kind{};

        // The appearance the finding is about: the one that misfired, the one
        // the fold wrongly answered with, or the one whose lead was thin. Absent
        // when the finding is about an element with no appearance to name.
        std::optional<annotation::ResourceName> appearance{};

        // The appearance that came too close, on ThinSeparation only.
        std::optional<annotation::ResourceName> rival{};
    };

    // The enumerator's name, for a JSON answer and a label.
    [[nodiscard]]
    auto modelFindingKindName(ModelFindingKind kind) noexcept -> char const*;

    // Everything the measured grid says is wrong with the model. Empty is the
    // verdict "accepted": every appearance matched exactly the screens it is
    // authored for, the fold answered with the right one and the right
    // rectangle, and every owner led its rivals by the required factor.
    //
    // Pure over the grid, so it needs no document, no runtime, and no pixels --
    // which is what lets a test hold a whole matrix and mutate one property.
    // A search the policy stopped is not a failure: it is a measurement that did
    // not happen, and reporting it as a defect would let a short budget condemn
    // a sound model.
    [[nodiscard]]
    auto judgeModelCheck(ModelCheck const& check) -> std::vector<ModelFinding>;

    // Evaluates every element against every captured screen, once, and reports
    // both what each screen resolved to and how much room each element has.
    // This is the check an author cannot perform by eye: a mark always matches
    // the image it was cut from at a score of zero, so the only evidence that it
    // identifies one screen rather than another is how it scores on the others.
    //
    // liveFrameBytes is a PNG freshly captured from the running target, or empty
    // when there is none. It is evaluated as one more screen and folded into the
    // same margins, so the author reads one row per element rather than
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
