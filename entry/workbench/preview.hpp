#pragma once

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <cstddef>
#include <optional>
#include <span>
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
        std::optional<PreviewPageKind>    pageKind{};
        std::optional<annotation::PageId> resolvedPageId{};
        std::vector<PreviewAnchorRow>     anchorRows{};
        std::optional<PreviewStop>        pageStop{};

        std::optional<PreviewAnchorRow> actionEvidence{};
        std::optional<PreviewStop>      actionStop{};
    };

    // The pixel-comparison ceiling one search may spend, shared by the preview
    // and the whole-model check so both are bounded the same way. It matches the
    // 256 Mi-pixel order of magnitude the authoring compiler bounds its own work
    // with (k_maximumCompilationPixelWork in authoring-compiler.cpp). Hitting it
    // surfaces as a stop reason rather than a miss.
    inline constexpr auto k_recognitionComparisonBudget = uint64{256} * 1024U * 1024U;

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
