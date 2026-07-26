#include "preview.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/recognition.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/error/result.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
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
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
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
                SessionId{1},
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
        [[nodiscard]]
        auto evaluatePageOn(
            annotation::RecognitionRuntime& runtime,
            Frame const& frame,
            annotation::ProjectFingerprint fingerprint,
            annotation::RecognitionPolicy const& policy
        ) -> Result<PreviewResult>
        {
            UF_TRY_VALUE(
                pageAttempt,
                runtime.evaluatePage(frame, fingerprint, policy)
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
            annotation::RecognizerId            recognizerId;
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

                // An interactive region belongs to the page it is authorized on;
                // a mark belongs to the page whose signature names it.
                for (auto const& pageId : recognizer.allowedPageIds())
                {
                    working = screenOfPage(pageId);
                    if (working.has_value())
                    {
                        break;
                    }
                }
                if (!working.has_value())
                {
                    for (auto const& page : document.catalog().pages())
                    {
                        auto const named =
                            std::ranges::contains(page.required(), recognizer.id())
                            || std::ranges::contains(page.forbidden(), recognizer.id());
                        if (named)
                        {
                            working = screenOfPage(page.id());
                            if (working.has_value())
                            {
                                break;
                            }
                        }
                    }
                }
                if (!working.has_value())
                {
                    auto const authored = std::ranges::find(
                        document.recognizerSources(),
                        recognizer.id(),
                        &annotation::AuthoringRecognizerSource::recognizerId
                    );
                    if (authored != document.recognizerSources().end())
                    {
                        working = authored->sourceId;
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
            std::span<annotation::RecognizerId const> actionIds,
            annotation::RecognitionPolicy const& policy
        ) -> Result<std::vector<PreviewAnchorRow>>
        {
            auto rows = std::vector<PreviewAnchorRow>{};
            rows.reserve(actionIds.size());
            for (auto const& actionId : actionIds)
            {
                UF_TRY_VALUE(
                    attempt,
                    runtime.evaluateActionTarget(
                        frame,
                        fingerprint,
                        actionId,
                        policy
                    )
                );
                if (
                    auto const* p_evidence = std::get_if<annotation::AnchorEvidence>(
                        &attempt.result
                    )
                )
                {
                    rows.emplace_back(toAnchorRow(*p_evidence));
                }
            }
            return rows;
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

    auto runPreview(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::SourceId selectedSourceId,
        std::optional<annotation::RecognizerId> selectedRecognizerId,
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

        if (selectedRecognizerId.has_value())
        {
            auto const* p_recognizer = document.catalog().findRecognizer(
                *selectedRecognizerId
            );
            if (
                p_recognizer != nullptr
                && p_recognizer->annotationType() == annotation::AnnotationType::ActionTarget
            )
            {
                UF_TRY_VALUE(
                    actionAttempt,
                    runtime.evaluateActionTarget(
                        frame,
                        fingerprint,
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
                        .recognizerId = p_actionStop->recognizerId,
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

        return result;
    }

    auto scoreRegionOnScreen(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        annotation::RecognizerId recognizerId,
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
            || p_recognizer->annotationType() != annotation::AnnotationType::ActionTarget
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "scoring a region requires an interactive region"
            );
        }

        UF_TRY_VALUE(runtime, buildRuntime(document, sourceAssets));
        auto const fingerprint = document.catalog().fingerprint();
        UF_TRY_VALUE(frame, previewFrame(fingerprint, screen->pngBytes));
        UF_TRY_VALUE(
            attempt,
            runtime.evaluateActionTarget(frame, fingerprint, recognizerId, policy)
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

    auto runModelCheck(
        annotation::AuthoringDocument const& document,
        std::span<annotation::AuthoringSourceAsset const> sourceAssets,
        std::span<std::byte const> liveFrameBytes,
        annotation::RecognitionPolicy const& policy
    ) -> Result<ModelCheck>
    {
        UF_TRY_VALUE(runtime, buildRuntime(document, sourceAssets));
        auto const fingerprint = document.catalog().fingerprint();

        auto actionIds = std::vector<annotation::RecognizerId>{};
        for (auto const& recognizer : document.catalog().recognizers())
        {
            if (
                recognizer.annotationType() == annotation::AnnotationType::ActionTarget
            )
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

            UF_TRY_VALUE(
                actionRows,
                evaluateActionsOn(
                    runtime,
                    frame,
                    fingerprint,
                    actionIds,
                    screenPolicy
                )
            );
            for (auto const& row : actionRows)
            {
                recordMargin(check.margins, working, asset.id, row);
            }
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

        UF_TRY_VALUE(
            liveActionRows,
            evaluateActionsOn(
                runtime,
                liveFrame,
                fingerprint,
                actionIds,
                livePolicy
            )
        );
        for (auto const& row : liveActionRows)
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
