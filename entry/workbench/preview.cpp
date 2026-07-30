#include "preview.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/safety/checked-access.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>
#include <domain/ids.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_rgbaBytesPerPixel = std::size_t{4};

        [[nodiscard]]
        auto toAnchorRow(annotation::AnchorEvidence const& evidence) -> PreviewAnchorRow
        {
            return PreviewAnchorRow{
                .recognizerId = evidence.recognizerId(),
                .hit          = evidence.hit(),
                .sadScore     = evidence.sadScore(),
                .maximumSad   = evidence.maximumSad(),
                .matchedRect  = evidence.matchedRect(),
            };
        }

        [[nodiscard]]
        auto toPageKind(annotation::PageOutcome const& outcome) noexcept -> PreviewPageKind
        {
            if (std::holds_alternative<annotation::ResolvedPage>(outcome))
            {
                return PreviewPageKind::Resolved;
            }
            if (std::holds_alternative<annotation::UnknownPage>(outcome))
            {
                return PreviewPageKind::Unknown;
            }
            return PreviewPageKind::Ambiguous;
        }

        [[nodiscard]]
        auto previewFrame(
            annotation::ProjectFingerprint fingerprint,
            std::span<std::byte const> pngBytes
        ) -> Result<Frame>
        {
            UF_TRY_VALUE(
                decoded,
                image::decodePng(pngBytes, "workbench-preview-source.png")
            );
            if (
                decoded.width != fingerprint.width()
                || decoded.height != fingerprint.height()
            )
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "preview source geometry does not match the project fingerprint"
                );
            }
            UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));
            UF_TRY_VALUE(
                transform,
                CoordinateTransform::create(
                    Point<DesktopSpace>{0.0F, 0.0F},
                    static_cast<float>(fingerprint.width()),
                    static_cast<float>(fingerprint.height()),
                    fingerprint.width(),
                    fingerprint.height()
                )
            );

            auto const width = checkedCast<std::size_t>(fingerprint.width());
            if (!width.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "preview source width is not addressable"
                );
            }
            auto const stride = checkedMultiply(
                *width,
                bytesPerPixel(PixelFormat::Bgra8)
            );
            if (!stride.has_value())
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    "preview source stride overflowed addressable memory"
                );
            }

            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(bgra))
            };
            return Frame::create(
                FrameId{1},
                CaptureSessionId{1},
                TargetGeneration::fromValue(1),
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                fingerprint.width(),
                fingerprint.height(),
                *stride,
                PixelFormat::Bgra8,
                buffer,
                transform
            );
        }

        // Compiles the document and stands up the runtime that evaluates against
        // it. Both callers pay this once: the model check evaluates every screen
        // through a single runtime rather than recompiling per screen.
        [[nodiscard]]
        auto buildRuntime(
            annotation::AuthoringDocument const& document,
            std::span<annotation::AuthoringSourceAsset const> sourceAssets
        ) -> Result<annotation::RecognitionRuntime>
        {
            UF_TRY_VALUE(
                compiled,
                annotation::compileAuthoringDocument(document, sourceAssets)
            );
            auto encodedTemplates = std::vector<annotation::EncodedRuntimeTemplate>{};
            encodedTemplates.reserve(compiled.templateAssets.size());
            for (auto& asset : compiled.templateAssets)
            {
                encodedTemplates.emplace_back(
                    annotation::EncodedRuntimeTemplate{
                        .hash     = asset.hash,
                        .pngBytes = std::move(asset.pngBytes),
                    }
                );
            }
            return annotation::RecognitionRuntime::create(
                std::move(compiled.runtimeManifest),
                std::move(encodedTemplates)
            );
        }

        // The page half of a preview: every anchor's evidence and how the page
        // classified. Shared so the model check evaluates each screen once,
        // rather than once per recognizer it wants a score for.
        //
        // The policy arrives carrying the per-search budget. evaluatePage shares
        // its budget across every anchor, so it is scaled here by the runtime's
        // anchor count -- the same page-anchor order evaluatePage iterates -- so
        // each anchor keeps a full per-search allowance rather than the first
        // large search spending the whole page's total. This is the one place
        // every page evaluation passes through, so scaling here covers the
        // preview, every captured screen, and the live frame alike.
        [[nodiscard]]
        auto evaluatePageOn(
            annotation::RecognitionRuntime& runtime,
            Frame const& frame,
            annotation::ProjectFingerprint fingerprint,
            annotation::RecognitionPolicy const& policy
        ) -> Result<PreviewResult>
        {
            auto const anchorCount =
                runtime.manifest().catalog().pageAnchorOrder().size();
            auto const pagePolicy = pagePolicyFor(policy, anchorCount);
            UF_TRY_VALUE(
                pageAttempt,
                runtime.evaluatePage(frame, fingerprint, pagePolicy)
            );

            auto result = PreviewResult{};
            result.anchorRows.reserve(pageAttempt.completedAnchorEvidence.size());
            for (auto const& evidence : pageAttempt.completedAnchorEvidence)
            {
                result.anchorRows.emplace_back(toAnchorRow(evidence));
            }
            if (
                auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                    &pageAttempt.result
                )
            )
            {
                result.pageStop = PreviewStop{
                    .recognizerId = p_stop->recognizerId,
                    .reason       = p_stop->reason,
                };
                return result;
            }

            auto const& outcome = std::get<annotation::PageOutcome>(
                pageAttempt.result
            );
            result.pageKind = toPageKind(outcome);
            if (
                auto const* p_resolved = std::get_if<annotation::ResolvedPage>(&outcome)
            )
            {
                result.resolvedPageId = p_resolved->pageId();
            }
            return result;
        }

        // The page a regression case says this screen stands for. Nothing else in
        // the document records it.
        [[nodiscard]]
        auto expectedPageOf(
            annotation::AuthoringDocument const& document,
            annotation::SourceId sourceId
        ) -> std::optional<annotation::PageId>
        {
            for (auto const& regression : document.regressions())
            {
                if (regression.sourceId() != sourceId)
                {
                    continue;
                }
                if (
                    auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                        &regression.expectation()
                    )
                )
                {
                    return p_resolved->pageId;
                }
            }
            return std::nullopt;
        }

        // The page whose reference supplies an element's interact search.
        //
        // One element compiles to one recognizer under its own id, so what a
        // page still decides is the region searched and any pinned appearance,
        // and evaluateActionTarget reads both off the reference. The screen
        // being searched and the page supplying the reference are therefore
        // independent: pageContext wins when that page exercises interact, and
        // otherwise the element's own page does. That is what keeps the grid's
        // off-diagonal cells measurable -- an element is searched on screens it
        // does not belong to, with the region its own page gives it, and a hit
        // there is the misfire the whole check exists to catch.
        //
        // Absent only for an element no page exercises interact on, which the
        // document's closure rule already forbids for anything declaring it.
        [[nodiscard]]
        auto interactPageFor(
            annotation::AuthoringDocument const& document,
            annotation::ElementId elementId,
            std::optional<annotation::PageId> pageContext
        ) -> std::optional<annotation::PageId>
        {
            auto fallback = std::optional<annotation::PageId>{};
            for (auto const& reference : document.references())
            {
                if (
                    reference.elementId != elementId
                    || !reference.exercised.hasInteract()
                )
                {
                    continue;
                }
                if (pageContext.has_value() && reference.pageId == *pageContext)
                {
                    return reference.pageId;
                }
                if (!fallback.has_value())
                {
                    fallback = reference.pageId;
                }
            }
            return fallback;
        }

        [[nodiscard]]
        auto screenOutcome(
            std::optional<annotation::PageId> expected,
            PreviewResult const& preview
        ) noexcept -> ScreenCheckOutcome
        {
            if (preview.pageStop.has_value() || !preview.pageKind.has_value())
            {
                return ScreenCheckOutcome::Stopped;
            }
            switch (*preview.pageKind)
            {
            case PreviewPageKind::Unknown:
                return ScreenCheckOutcome::Unknown;
            case PreviewPageKind::Ambiguous:
                return ScreenCheckOutcome::Ambiguous;
            case PreviewPageKind::Resolved:
                break;
            }
            if (!expected.has_value())
            {
                return ScreenCheckOutcome::Unclaimed;
            }
            return preview.resolvedPageId == expected
                ? ScreenCheckOutcome::Correct
                : ScreenCheckOutcome::WrongPage;
        }

        // Which screen one recognizer has to work on, paired with its identity.
        struct WorkingScreen final
        {
            annotation::ElementId               recognizerId;
            std::optional<annotation::SourceId> sourceId{};
        };

        // The screen each recognizer is searched on at runtime: the screen
        // recorded for the page it belongs to.
        //
        // For anything drawn on the page it serves this is also the screen its
        // template was cut from, and the distinction never shows. A shared
        // element is the exception -- cut from one screen, used on another -- and
        // reading its score against the screen it was cut from would report a
        // perfect match for a recognizer that never fires anywhere it is
        // authorized. The page's screen is the only one that answers the question
        // the author is asking.
        //
        // Falls back to the screen the template was cut from when no page claims
        // the recognizer or that page records no screen.
        [[nodiscard]]
        auto workingScreens(
            annotation::AuthoringDocument const& document
        ) -> std::vector<WorkingScreen>
        {
            auto const screenOfPage = [&document](annotation::PageId pageId)
            {
                for (auto const& regression : document.regressions())
                {
                    auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                        &regression.expectation()
                    );
                    if (p_resolved != nullptr && p_resolved->pageId == pageId)
                    {
                        return std::optional<annotation::SourceId>{
                            regression.sourceId()
                        };
                    }
                }
                return std::optional<annotation::SourceId>{};
            };

            auto screens = std::vector<WorkingScreen>{};
            screens.reserve(document.catalog().recognizers().size());
            for (auto const& recognizer : document.catalog().recognizers())
            {
                auto working = std::optional<annotation::SourceId>{};

                // Every page-side use is a reference now, whichever capability
                // it exercises, so one pass covers the page that clicks the
                // element and the page it identifies as well.
                for (auto const& reference : document.references())
                {
                    if (reference.elementId != recognizer.id())
                    {
                        continue;
                    }
                    working = screenOfPage(reference.pageId);
                    if (working.has_value())
                    {
                        break;
                    }
                }
                if (!working.has_value())
                {
                    auto const* p_element = document.findElement(recognizer.id());
                    if (p_element != nullptr && !p_element->variants().empty())
                    {
                        working = p_element->variants().front().sourceId();
                    }
                }

                screens.emplace_back(
                    WorkingScreen{
                        .recognizerId = recognizer.id(),
                        .sourceId     = working,
                    }
                );
            }
            return screens;
        }

        // Files one score under the recognizer it belongs to, keeping the score
        // on the screen it has to work on and the lowest score on any other. The
        // lowest is the interesting one: it is the screen this mark comes closest
        // to claiming by mistake.
        auto recordMargin(
            std::vector<RecognizerMargin>& margins,
            std::span<WorkingScreen const> working,
            annotation::SourceId sourceId,
            PreviewAnchorRow const& row
        ) -> void
        {
            auto found = std::ranges::find(
                margins,
                row.recognizerId,
                &RecognizerMargin::recognizerId
            );
            if (found == margins.end())
            {
                auto const entry = std::ranges::find(
                    working,
                    row.recognizerId,
                    &WorkingScreen::recognizerId
                );
                margins.emplace_back(
                    RecognizerMargin{
                        .recognizerId = row.recognizerId,
                        .maximumSad   = row.maximumSad,
                        .ownSourceId  = entry == working.end()
                            ? std::optional<annotation::SourceId>{}
                            : entry->sourceId,
                    }
                );
                found = std::prev(margins.end());
            }

            if (!row.sadScore.has_value())
            {
                return;
            }
            if (found->ownSourceId == sourceId)
            {
                found->ownSadScore = row.sadScore;
                return;
            }
            if (
                !found->nearestOtherSadScore.has_value()
                || *row.sadScore < *found->nearestOtherSadScore
            )
            {
                found->nearestOtherSadScore = row.sadScore;
                found->nearestOtherSourceId = sourceId;
            }
        }

        // Files a score measured on the running target. The live frame is
        // evaluated after every captured screen, so each recognizer already has
        // an entry; a frame nobody authored against introduces no new ones.
        auto recordLiveMargin(
            std::vector<RecognizerMargin>& margins,
            PreviewAnchorRow const& row
        ) -> void
        {
            auto const found = std::ranges::find(
                margins,
                row.recognizerId,
                &RecognizerMargin::recognizerId
            );
            if (found != margins.end())
            {
                found->liveSadScore = row.sadScore;
            }
        }

        // Whether an element is authored to match on a page. The reference IS
        // that relation now -- membership, authorisation, and a page's signature
        // are all the same edge -- so one lookup answers what three used to.
        // This is the ground truth the grid's colour reads against: a hit on a
        // page that references the element is expected, a hit anywhere else is a
        // misfire.
        [[nodiscard]]
        auto elementBelongsToPage(
            annotation::AuthoringDocument const& document,
            annotation::ElementId elementId,
            annotation::PageId pageId
        ) -> bool
        {
            return document.catalog().findReference(pageId, elementId) != nullptr;
        }

        // One action search on one screen: the element to evaluate, paired with
        // the page whose reference supplies the region it is searched in. The
        // page is not the screen's -- see interactPageFor -- so the same element
        // is measured on every screen, which is what the grid needs.
        struct ActionSearch final
        {
            annotation::PageId    pageId;
            annotation::ElementId elementId;
        };

        // An action search the policy interrupted before it produced evidence,
        // named by the element id (never the derived per-page id) and the reason.
        // The margins fold a stopped search away as an absent row; the grid needs
        // it as an explicit Stopped cell, so it is kept alongside the rows here.
        struct ActionStop final
        {
            annotation::ElementId elementId;
            SadSearchStopReason   reason{};
        };

        // Every action target's evidence on one frame, with the searches the
        // policy stopped kept apart. The rows feed the margins exactly as before;
        // the stops let the grid tell a stopped search from one not run here.
        struct ActionEvaluation final
        {
            std::vector<PreviewAnchorRow> rows{};
            std::vector<ActionStop>       stops{};
        };

        // The action searches to run on one screen, one per action element that
        // any page exercises interact on. pageContext -- the page recorded for
        // the screen, when it has one -- only decides WHICH reference supplies
        // the region; it never decides whether the element is searched, because
        // a mark that also matches a foreign screen is precisely the misfire the
        // check is for and it can only be seen by looking there.
        [[nodiscard]]
        auto actionSearchesOn(
            annotation::AuthoringDocument const& document,
            std::span<annotation::ElementId const> actionElementIds,
            std::optional<annotation::PageId> pageContext
        ) -> std::vector<ActionSearch>
        {
            auto searches = std::vector<ActionSearch>{};
            searches.reserve(actionElementIds.size());
            for (auto const& elementId : actionElementIds)
            {
                auto const pageId = interactPageFor(
                    document,
                    elementId,
                    pageContext
                );
                if (pageId.has_value())
                {
                    searches.emplace_back(
                        ActionSearch{
                            .pageId    = *pageId,
                            .elementId = elementId,
                        }
                    );
                }
            }
            return searches;
        }

        // Every action target's evidence on one frame. Action targets take no
        // part in resolving the page, so evaluatePage never scores them; they are
        // searched for the same reason the anchors are, because a button template
        // that also matches another screen is a misfire waiting for the page to
        // resolve there. A target the policy stopped on contributes no row rather
        // than a score that was never measured.
        [[nodiscard]]
        auto evaluateActionsOn(
            annotation::RecognitionRuntime& runtime,
            Frame const& frame,
            annotation::ProjectFingerprint fingerprint,
            std::span<ActionSearch const> searches,
            annotation::RecognitionPolicy const& policy
        ) -> Result<ActionEvaluation>
        {
            auto evaluation = ActionEvaluation{};
            evaluation.rows.reserve(searches.size());
            for (auto const& search : searches)
            {
                UF_TRY_VALUE(
                    attempt,
                    runtime.evaluateActionTarget(
                        frame,
                        fingerprint,
                        search.pageId,
                        search.elementId,
                        policy
                    )
                );
                if (
                    auto const* p_evidence = std::get_if<annotation::AnchorEvidence>(
                        &attempt.result
                    )
                )
                {
                    evaluation.rows.emplace_back(toAnchorRow(*p_evidence));
                }
                else if (
                    auto const* p_stop = std::get_if<annotation::PageRecognitionStop>(
                        &attempt.result
                    )
                )
                {
                    evaluation.stops.emplace_back(
                        ActionStop{
                            .elementId = search.elementId,
                            .reason    = p_stop->reason,
                        }
                    );
                }
            }
            return evaluation;
        }

        // The grid cells for one screen, derived from the evaluation the margins
        // already fold in -- the page anchor rows (and any page stop) and the
        // action rows (and any action stop) -- so no element is searched twice.
        //
        // An anchor is scored on every screen by the page evaluation, so a row is
        // its measured outcome; a missing row means the page stopped before
        // reaching it (Stopped) or no page exercises identify on it and it was
        // never scored (NotSearchedHere). An action element is searched on every
        // screen too, through the reference its own page gives it, so its only
        // NotSearchedHere is the same absence: no page exercises it at all.
        [[nodiscard]]
        auto deriveScreenCells(
            annotation::AuthoringDocument const& document,
            annotation::SourceId screenId,
            std::optional<annotation::PageId> expectedPage,
            std::span<annotation::ElementId const> anchorIds,
            std::span<annotation::ElementId const> actionIds,
            PreviewResult const& preview,
            std::span<ActionSearch const> searches,
            ActionEvaluation const& actionEval
        ) -> std::vector<ModelCheckCell>
        {
            auto const belongs = [&](annotation::ElementId elementId)
            {
                return expectedPage.has_value()
                    && elementBelongsToPage(document, elementId, *expectedPage);
            };

            auto cells = std::vector<ModelCheckCell>{};
            cells.reserve(anchorIds.size() + actionIds.size());

            for (auto const& anchorId : anchorIds)
            {
                auto const row = std::ranges::find(
                    preview.anchorRows,
                    anchorId,
                    &PreviewAnchorRow::recognizerId
                );
                if (row != preview.anchorRows.end())
                {
                    cells.emplace_back(
                        ModelCheckCell{
                            .elementId = anchorId,
                            .screenId  = screenId,
                            .outcome     = row->hit
                                ? ModelCellOutcome::Hit
                                : ModelCellOutcome::Miss,
                            .sadScore    = row->sadScore,
                            .maximumSad  = row->maximumSad,
                            .expectedHit = belongs(anchorId),
                        }
                    );
                    continue;
                }
                // No row and the page stopped: the anchor never got its turn. No
                // row and the page did not stop: it is on no page and was not
                // scored here at all.
                cells.emplace_back(
                    ModelCheckCell{
                        .elementId = anchorId,
                        .screenId  = screenId,
                        .outcome     = preview.pageStop.has_value()
                            ? ModelCellOutcome::Stopped
                            : ModelCellOutcome::NotSearchedHere,
                        .expectedHit = belongs(anchorId),
                        .stopReason  = preview.pageStop.has_value()
                            ? std::optional<SadSearchStopReason>{
                                preview.pageStop->reason
                            }
                            : std::nullopt,
                    }
                );
            }

            for (auto const& actionId : actionIds)
            {
                auto const searched = std::ranges::any_of(
                    searches,
                    [&](ActionSearch const& search)
                    {
                        return search.elementId == actionId;
                    }
                );
                if (!searched)
                {
                    cells.emplace_back(
                        ModelCheckCell{
                            .elementId = actionId,
                            .screenId  = screenId,
                            .outcome   = ModelCellOutcome::NotSearchedHere,
                        }
                    );
                    continue;
                }
                auto const row = std::ranges::find(
                    actionEval.rows,
                    actionId,
                    &PreviewAnchorRow::recognizerId
                );
                if (row != actionEval.rows.end())
                {
                    cells.emplace_back(
                        ModelCheckCell{
                            .elementId = actionId,
                            .screenId  = screenId,
                            .outcome     = row->hit
                                ? ModelCellOutcome::Hit
                                : ModelCellOutcome::Miss,
                            .sadScore    = row->sadScore,
                            .maximumSad  = row->maximumSad,
                            .expectedHit = belongs(actionId),
                        }
                    );
                    continue;
                }
                auto const stop = std::ranges::find(
                    actionEval.stops,
                    actionId,
                    &ActionStop::elementId
                );
                cells.emplace_back(
                    ModelCheckCell{
                        .elementId   = actionId,
                        .screenId    = screenId,
                        .outcome     = ModelCellOutcome::Stopped,
                        .expectedHit = belongs(actionId),
                        .stopReason  = stop != actionEval.stops.end()
                            ? std::optional<SadSearchStopReason>{stop->reason}
                            : std::nullopt,
                    }
                );
            }
            return cells;
        }

        // Splits what is left of the run's deadline evenly across the screens
        // still to be checked, so every screen gets the same share of the clock.
        // Under one shared deadline the screens reached first spend it and the
        // rest report Stopped, which reads in the Pages panel exactly like a
        // screen that failed to resolve -- the two mean opposite things. Time a
        // screen does not use rolls forward to the ones after it.
        //
        // Overflow falls back to the run deadline rather than to no deadline:
        // an unbounded search is the failure this whole split exists to avoid.
        [[nodiscard]]
        auto screenDeadline(
            std::optional<MonotonicInstant> runDeadline,
            std::size_t remainingScreens
        ) -> std::optional<MonotonicInstant>
        {
            if (!runDeadline.has_value() || remainingScreens <= 1U)
            {
                return runDeadline;
            }

            auto const now = MonotonicInstant::now();
            if (now >= *runDeadline)
            {
                return runDeadline;
            }

            auto const remaining = runDeadline->saturatingDurationSince(now);
            auto const divisor   = checkedCast<MonotonicInstant::Duration::rep>(
                remainingScreens
            );
            if (!divisor || *divisor == 0)
            {
                return runDeadline;
            }

            auto const share = MonotonicInstant::Duration{remaining.count() / *divisor};
            return now.checkedAdd(share).value_or(*runDeadline);
        }
    }

    auto pagePolicyFor(
        annotation::RecognitionPolicy const& perSearchPolicy,
        std::size_t anchorSearchCount
    ) -> annotation::RecognitionPolicy
    {
        auto scaled       = perSearchPolicy;
        auto const searches = checkedCast<uint64>(anchorSearchCount);
        auto const total    = searches.has_value()
            ? checkedMultiply(perSearchPolicy.maximumPixelComparisons, *searches)
            : std::optional<uint64>{};

        // Overflow (or an unaddressable count) saturates to the largest ceiling
        // we can name rather than falling back below the per-search intent: a
        // shrunk budget is exactly the starvation this scaling removes.
        scaled.maximumPixelComparisons = total.value_or(
            std::numeric_limits<uint64>::max()
        );
        return scaled;
    }

    auto classifyModelCell(ModelCheckCell const& cell) noexcept -> ModelCellColor
    {
        switch (cell.outcome)
        {
        case ModelCellOutcome::NotSearchedHere:
            return ModelCellColor::NotSearched;
        case ModelCellOutcome::Stopped:
            return ModelCellColor::Thin;
        case ModelCellOutcome::Hit:
            // A hit is expected only where the element is authored to match; a
            // hit anywhere else is the misfire the whole check exists to catch.
            if (!cell.expectedHit)
            {
                return ModelCellColor::Misfire;
            }
            break;
        case ModelCellOutcome::Miss:
            // A miss where the element's own page is recorded for the screen is a
            // hole -- a mark that should identify this screen and does not.
            if (cell.expectedHit)
            {
                return ModelCellColor::Misfire;
            }
            break;
        }

        // The outcome is the expected one; a margin within the thin band still
        // reads amber, because a mark that only just passed or just failed is a
        // frame of drift from flipping. Overflow scaling the distance means the
        // score is nowhere near the threshold, so it is not thin.
        if (cell.sadScore.has_value() && cell.maximumSad > 0U)
        {
            auto const score    = *cell.sadScore;
            auto const distance = score >= cell.maximumSad
                ? score - cell.maximumSad
                : cell.maximumSad - score;
            auto const scaled = checkedMultiply(distance, k_thinMarginDenominator);
            if (
                scaled.has_value()
                && *scaled <= k_thinMarginNumerator * cell.maximumSad
            )
            {
                return ModelCellColor::Thin;
            }
        }
        return ModelCellColor::Expected;
    }

    auto runPreview(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::SourceId selectedSourceId,
        std::optional<annotation::ElementId> selectedRecognizerId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewResult>
    {
        auto const selected = std::ranges::find(
            sourceAssets,
            selectedSourceId,
            &annotation::AuthoringSourceAsset::id
        );
        if (selected == sourceAssets.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "preview requires the selected source to be present in the project"
            );
        }

        UF_TRY_VALUE(runtime, buildRuntime(document, sourceAssets));

        auto const fingerprint = document.catalog().fingerprint();
        UF_TRY_VALUE(frame, previewFrame(fingerprint, selected->pngBytes));
        UF_TRY_VALUE(result, evaluatePageOn(runtime, frame, fingerprint, policy));
        result.sourceId = selectedSourceId;

        if (selectedRecognizerId.has_value())
        {
            auto const* p_recognizer = document.catalog().findRecognizer(
                *selectedRecognizerId
            );
            if (
                p_recognizer != nullptr
                && p_recognizer->capabilities().hasInteract()
            )
            {
                // The element is one recognizer under the id the UI selected it
                // by, so the only thing the shown screen's page decides is which
                // reference supplies the search region. When it decides nothing
                // -- an unclaimed screen -- the element's own page does, and the
                // search still runs: what an author looking at a foreign screen
                // wants to know is precisely whether these pixels turn up there.
                auto const pageContext = expectedPageOf(document, selectedSourceId);
                auto const searchPage  = interactPageFor(
                    document,
                    *selectedRecognizerId,
                    pageContext
                );
                if (searchPage.has_value())
                {
                    UF_TRY_VALUE(
                        actionAttempt,
                        runtime.evaluateActionTarget(
                            frame,
                            fingerprint,
                            *searchPage,
                            *selectedRecognizerId,
                            policy
                        )
                    );
                    if (
                        auto const* p_actionStop = std::get_if<annotation::PageRecognitionStop>(
                            &actionAttempt.result
                        )
                    )
                    {
                        result.actionStop = PreviewStop{
                            .recognizerId = *selectedRecognizerId,
                            .reason       = p_actionStop->reason,
                        };
                    }
                    else
                    {
                        result.actionEvidence = toAnchorRow(
                            std::get<annotation::AnchorEvidence>(actionAttempt.result)
                        );
                    }
                }
            }
        }

        return result;
    }

    auto scoreRegionOnScreen(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::ElementId recognizerId,
        annotation::SourceId screenId,
        annotation::RecognitionPolicy const& policy
    ) -> Result<PreviewAnchorRow>
    {
        auto const screen = std::ranges::find(
            sourceAssets,
            screenId,
            &annotation::AuthoringSourceAsset::id
        );
        if (screen == sourceAssets.end())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "scoring a region requires the screen to be part of the project"
            );
        }
        auto const* p_recognizer = document.catalog().findRecognizer(recognizerId);
        if (
            p_recognizer == nullptr
            || !p_recognizer->capabilities().hasInteract()
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "scoring a region requires an interactive region"
            );
        }
        auto const searchPage = interactPageFor(
            document,
            recognizerId,
            std::nullopt
        );
        if (!searchPage.has_value())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "no page exercises this region, so there is no region to search"
            );
        }

        UF_TRY_VALUE(runtime, buildRuntime(document, sourceAssets));
        auto const fingerprint = document.catalog().fingerprint();
        UF_TRY_VALUE(frame, previewFrame(fingerprint, screen->pngBytes));
        UF_TRY_VALUE(
            attempt,
            runtime.evaluateActionTarget(
                frame,
                fingerprint,
                *searchPage,
                recognizerId,
                policy
            )
        );

        auto const* p_evidence = std::get_if<annotation::AnchorEvidence>(
            &attempt.result
        );
        if (p_evidence == nullptr)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "the search hit its budget before scoring the region"
            );
        }
        return toAnchorRow(*p_evidence);
    }

    auto previewColourKeyMask(
        annotation::AuthoringSourceAsset const& asset,
        PixelRect templateRect,
        std::optional<annotation::ColourKey> colourKey
    ) -> Result<ColourKeyMaskPreview>
    {
        UF_TRY_VALUE(
            decoded,
            image::decodePng(asset.pngBytes, "workbench-colour-key-source.png")
        );
        UF_TRY(templateRect.ensureWithinExtent(decoded.width, decoded.height));

        auto const sourceStride = checkedMultiply(
            static_cast<std::size_t>(decoded.width),
            k_rgbaBytesPerPixel
        );
        auto const rowBytes = checkedMultiply(
            static_cast<std::size_t>(templateRect.width()),
            k_rgbaBytesPerPixel
        );
        auto maskBytes = std::optional<std::size_t>{};
        if (rowBytes)
        {
            maskBytes = checkedMultiply(
                *rowBytes,
                static_cast<std::size_t>(templateRect.height())
            );
        }
        if (!sourceStride || !rowBytes || !maskBytes)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "colour key mask preview overflowed addressable memory"
            );
        }

        auto preview = ColourKeyMaskPreview{
            .width       = templateRect.width(),
            .height      = templateRect.height(),
            .rgbaPixels  = std::vector<std::byte>(*maskBytes, std::byte{0}),
            .totalPixels = *maskBytes / k_rgbaBytesPerPixel,
        };
        for (auto row = uint32{0}; row < templateRect.height(); ++row)
        {
            auto const sourceRow = (
                (static_cast<std::size_t>(templateRect.y()) + row) * *sourceStride
                + static_cast<std::size_t>(templateRect.x()) * k_rgbaBytesPerPixel
            );
            auto const maskRow = static_cast<std::size_t>(row) * *rowBytes;
            for (auto column = uint32{0}; column < templateRect.width(); ++column)
            {
                auto const from = sourceRow
                    + static_cast<std::size_t>(column) * k_rgbaBytesPerPixel;
                auto const into = maskRow
                    + static_cast<std::size_t>(column) * k_rgbaBytesPerPixel;
                auto const red   = std::to_integer<uint8>(checkedAt(decoded.pixels, from));
                auto const green = std::to_integer<uint8>(
                    checkedAt(decoded.pixels, from + 1U)
                );
                auto const blue = std::to_integer<uint8>(
                    checkedAt(decoded.pixels, from + 2U)
                );
                auto const alpha = colourKey
                    ? colourKey->alphaFor(red, green, blue)
                    : uint8{255};

                checkedAt(preview.rgbaPixels, into)      = std::byte{red};
                checkedAt(preview.rgbaPixels, into + 1U) = std::byte{green};
                checkedAt(preview.rgbaPixels, into + 2U) = std::byte{blue};
                checkedAt(preview.rgbaPixels, into + 3U) = std::byte{alpha};

                if (alpha == 255U)
                {
                    ++preview.fullyKeptPixels;
                }
                else if (alpha != 0U)
                {
                    ++preview.partiallyKeptPixels;
                }
            }
        }
        return preview;
    }

    auto sampleSourcePixel(
        annotation::AuthoringSourceAsset const& asset,
        PixelPoint point
    ) -> Result<SampledSourcePixel>
    {
        UF_TRY_VALUE(
            decoded,
            image::decodePng(asset.pngBytes, "workbench-colour-key-source.png")
        );
        if (point.x() >= decoded.width || point.y() >= decoded.height)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "the sampled pixel is outside the captured screen"
            );
        }

        auto const stride = checkedMultiply(
            static_cast<std::size_t>(decoded.width),
            k_rgbaBytesPerPixel
        );
        if (!stride)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "colour key source stride overflowed addressable memory"
            );
        }
        auto const offset = static_cast<std::size_t>(point.y()) * *stride
            + static_cast<std::size_t>(point.x()) * k_rgbaBytesPerPixel;
        return SampledSourcePixel{
            .red   = std::to_integer<uint8>(checkedAt(decoded.pixels, offset)),
            .green = std::to_integer<uint8>(checkedAt(decoded.pixels, offset + 1U)),
            .blue  = std::to_integer<uint8>(checkedAt(decoded.pixels, offset + 2U)),
        };
    }

    auto runModelCheck(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        std::span<std::byte const> liveFrameBytes,
        annotation::RecognitionPolicy const& policy
    ) -> Result<ModelCheck>
    {
        UF_TRY_VALUE(runtime, buildRuntime(document, sourceAssets));
        auto const fingerprint = document.catalog().fingerprint();

        // The grid rows are the same set the margins cover, split by which pass
        // measures them rather than by a type the element no longer has. The
        // anchor pass scans exactly pageAnchorOrder, so an element in it is
        // already scored on every screen once -- searching it again as an action
        // would put back the second search per cycle the capability set exists
        // to remove. What is left with an interact capability is searched per
        // screen. An element that is only read takes part in neither.
        auto const& catalog = document.catalog();
        auto const scannedByPagePass = [&catalog](annotation::ElementId id)
        {
            return std::ranges::contains(catalog.pageAnchorOrder(), id);
        };

        auto anchorIds = std::vector<annotation::ElementId>{};
        auto actionIds = std::vector<annotation::ElementId>{};
        for (auto const& recognizer : catalog.recognizers())
        {
            // Declares identify but no page exercises it: nothing scores it, and
            // its row stays as an explicit not-searched column rather than
            // vanishing from the grid.
            auto const unexercisedMark = (
                recognizer.capabilities().hasIdentify()
                && !recognizer.capabilities().hasInteract()
            );
            if (scannedByPagePass(recognizer.id()) || unexercisedMark)
            {
                anchorIds.emplace_back(recognizer.id());
            }
            else if (recognizer.capabilities().hasInteract())
            {
                actionIds.emplace_back(recognizer.id());
            }
        }

        // Resolved once: which screen each recognizer is actually searched on.
        // For a shared element that is not the screen its template came from.
        auto const working = workingScreens(document);

        auto check = ModelCheck{};
        check.screens.reserve(sourceAssets.size());

        // The live frame is one more screen for the purpose of dividing the
        // clock, so counting it here keeps every screen's slice equal instead of
        // letting the captured ones spend the whole run.
        auto const hasLiveFrame = !liveFrameBytes.empty();
        auto remainingScreens   = sourceAssets.size() + (hasLiveFrame ? 1U : 0U);
        for (auto const& asset : sourceAssets)
        {
            // One screen's whole evaluation -- its page anchors and every action
            // target below -- shares this screen's slice of the run's clock.
            auto screenPolicy       = policy;
            screenPolicy.deadline = screenDeadline(
                policy.deadline,
                remainingScreens
            );
            remainingScreens -= 1U;

            UF_TRY_VALUE(frame, previewFrame(fingerprint, asset.pngBytes));
            UF_TRY_VALUE(
                preview,
                evaluatePageOn(runtime, frame, fingerprint, screenPolicy)
            );

            auto const expected = expectedPageOf(document, asset.id);
            check.screens.emplace_back(
                ScreenCheck{
                    .sourceId       = asset.id,
                    .expectedPageId = expected,
                    .resolvedPageId = preview.resolvedPageId,
                    .outcome        = screenOutcome(expected, preview),
                }
            );
            for (auto const& row : preview.anchorRows)
            {
                recordMargin(check.margins, working, asset.id, row);
            }

            // Every action element is searched on this screen. The page a
            // regression records for it is only the preferred supplier of the
            // search region; an element that page does not exercise is still
            // searched, with the region its own page gives it, because a hit
            // where it does not belong is what the grid is looking for.
            auto const actionSearches = actionSearchesOn(
                document,
                actionIds,
                expected
            );
            UF_TRY_VALUE(
                actionEval,
                evaluateActionsOn(
                    runtime,
                    frame,
                    fingerprint,
                    actionSearches,
                    screenPolicy
                )
            );
            for (auto const& row : actionEval.rows)
            {
                recordMargin(check.margins, working, asset.id, row);
            }

            // The full grid, from the same evaluation the margins just folded
            // in: no element is searched a second time to fill it.
            auto screenCells = deriveScreenCells(
                document,
                asset.id,
                expected,
                anchorIds,
                actionIds,
                preview,
                actionSearches,
                actionEval
            );
            check.cells.insert(
                check.cells.end(),
                std::make_move_iterator(screenCells.begin()),
                std::make_move_iterator(screenCells.end())
            );
        }

        if (!hasLiveFrame)
        {
            return check;
        }

        // The live frame is measured last, once every recognizer already has a
        // margin entry, and it is never added to the project: a frame taken to
        // measure against is not a screen the model is authored on.
        auto livePolicy     = policy;
        livePolicy.deadline = screenDeadline(policy.deadline, remainingScreens);

        UF_TRY_VALUE(liveFrame, previewFrame(fingerprint, liveFrameBytes));
        UF_TRY_VALUE(
            livePreview,
            evaluatePageOn(runtime, liveFrame, fingerprint, livePolicy)
        );
        check.live = LiveScreenCheck{
            .pageKind       = livePreview.pageKind,
            .resolvedPageId = livePreview.resolvedPageId,
            .stop           = livePreview.pageStop,
        };
        for (auto const& row : livePreview.anchorRows)
        {
            recordLiveMargin(check.margins, row);
        }

        // The live frame carries no recorded page, so the page it resolved to is
        // its only context: it decides which reference supplies each search
        // region, and an element that page does not exercise still gets a live
        // score through the reference its own page gives it.
        auto const liveActionSearches = actionSearchesOn(
            document,
            actionIds,
            livePreview.resolvedPageId
        );
        UF_TRY_VALUE(
            liveActionEval,
            evaluateActionsOn(
                runtime,
                liveFrame,
                fingerprint,
                liveActionSearches,
                livePolicy
            )
        );
        for (auto const& row : liveActionEval.rows)
        {
            recordLiveMargin(check.margins, row);
        }

        return check;
    }

    auto previewPageKindName(PreviewPageKind kind) noexcept -> char const*
    {
        switch (kind)
        {
        case PreviewPageKind::Resolved:
            return "Resolved";
        case PreviewPageKind::Unknown:
            return "Unknown";
        case PreviewPageKind::Ambiguous:
            return "Ambiguous";
        }
        return "?";
    }
}
