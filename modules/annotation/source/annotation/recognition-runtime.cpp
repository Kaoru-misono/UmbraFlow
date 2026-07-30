#include "recognition-runtime.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/space.hpp>

#include <image/pixels.hpp>
#include <image/png.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        [[nodiscard]]
        auto invalidRuntime(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto incompatibleFrame(std::string message) -> std::unexpected<Error>
        {
            return fail(
                AutomationErrorKind::TargetCompatibilityUnverified,
                std::move(message)
            );
        }

        [[nodiscard]]
        auto pageRecognitionFailure(
            PageRecognitionStop const& stop
        ) -> std::unexpected<Error>
        {
            return fail(
                searchStopKind(stop.reason),
                std::format(
                    "page recognition {} at anchor {}",
                    searchStopDescription(stop.reason),
                    stop.recognizerId.value().toString()
                )
            );
        }

        // Captured by value because the SAD matcher stores this poll and calls it
        // during the search; a stored callback must not borrow caller state.
        [[nodiscard]]
        auto makeSadSearchPoll(RecognitionPolicy const& policy) -> SadSearchPoll
        {
            auto const cancellation = policy.cancellation;
            auto const deadline     = policy.deadline;
            return SadSearchPoll{
                [cancellation, deadline]() noexcept -> SadSearchControl
                {
                    if (cancellation.stop_requested())
                    {
                        return SadSearchControl::Cancelled;
                    }
                    if (deadline && MonotonicInstant::now() >= *deadline)
                    {
                        return SadSearchControl::TimedOut;
                    }
                    return SadSearchControl::Continue;
                }
            };
        }

        // Converts the frame to a Gray8 view once and hands it to the
        // continuation while the backing storage is still alive. The Bgra8 path's
        // gray buffer lives on this stack frame for the whole synchronous call, so
        // the view the continuation receives never outlives its owner.
        template <typename Continuation>
        [[nodiscard]]
        auto withGrayFrame(
            Frame const& frame,
            Continuation const& continuation
        ) -> std::invoke_result_t<Continuation const&, GrayImage const&>
        {
            auto const p_pixels = frame.pixels();
            UF_CHECK(p_pixels != nullptr);
            switch (frame.pixelFormat())
            {
            case PixelFormat::Gray8:
            {
                UF_TRY_VALUE(
                    grayFrame,
                    GrayImage::create(
                        p_pixels->bytes(),
                        frame.width(),
                        frame.height(),
                        frame.stride()
                    )
                );
                return std::invoke(continuation, grayFrame);
            }
            case PixelFormat::Bgra8:
            {
                UF_TRY_VALUE(
                    grayPixels,
                    bgra8ToGray8(
                        p_pixels->bytes(),
                        frame.width(),
                        frame.height(),
                        frame.stride()
                    )
                );
                auto const grayStride = checkedCast<std::size_t>(frame.width());
                UF_CHECK(grayStride.has_value());
                UF_TRY_VALUE(
                    grayFrame,
                    GrayImage::create(
                        grayPixels,
                        frame.width(),
                        frame.height(),
                        *grayStride
                    )
                );
                return std::invoke(continuation, grayFrame);
            }
            }

            UF_UNREACHABLE_MSG("Unknown PixelFormat value");
        }

        // Variant scores are not comparable as they stand: maximumSad is a
        // function of each variant's own template size and threshold, so a raw
        // SAD from a 40x12 template says nothing beside one from a 90x20. The
        // exact integer comparison of two normalized margins is their cross
        // product, which needs no division and so loses nothing to truncation.
        //
        // Preference order: a hit beats a non-hit; between two of a kind the
        // lower normalized distance wins; a search that matched nothing at all
        // is last. A tie keeps the incumbent, which is the earlier declaration.
        // Declaration order decides ties and nothing else, because "first past
        // the threshold wins" would let a wide early variant answer for a
        // narrow later one and move the click to its own rectangle, with
        // nothing downstream able to notice.
        [[nodiscard]]
        auto strictlyBetter(
            AnchorEvidence const& candidate,
            AnchorEvidence const& incumbent
        ) -> Result<bool>
        {
            if (candidate.hit() != incumbent.hit())
            {
                return candidate.hit();
            }

            auto const candidateScore = candidate.sadScore();
            auto const incumbentScore = incumbent.sadScore();
            if (!candidateScore)
            {
                return false;
            }
            if (!incumbentScore)
            {
                return true;
            }

            auto const left  = checkedMultiply(*candidateScore, incumbent.maximumSad());
            auto const right = checkedMultiply(*incumbentScore, candidate.maximumSad());
            if (!left || !right)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "comparing two variant margins overflowed"
                );
            }
            return *left < *right;
        }
    }

    RecognitionRuntime::RecognitionRuntime(
        RuntimeManifest manifest,
        std::vector<GrayTemplate> templates
    ) noexcept
        : m_manifest{std::move(manifest)}
        , m_templates{std::move(templates)}
    {
    }

    auto RecognitionRuntime::create(
        RuntimeManifest manifest,
        std::vector<EncodedRuntimeTemplate> encodedTemplates
    ) -> Result<RecognitionRuntime>
    {
        std::ranges::sort(encodedTemplates, {}, &EncodedRuntimeTemplate::hash);
        for (auto index = std::size_t{1}; index < encodedTemplates.size(); ++index)
        {
            if (encodedTemplates[index - 1U].hash == encodedTemplates[index].hash)
            {
                return invalidRuntime(
                    std::format(
                        "duplicate encoded runtime template {}",
                        encodedTemplates[index].hash.toString()
                    )
                );
            }
        }

        auto expectedHashes = std::vector<ContentHash>{};
        expectedHashes.reserve(manifest.assets().size());
        for (auto const& asset : manifest.assets())
        {
            expectedHashes.emplace_back(asset.templateHash);
        }
        std::ranges::sort(expectedHashes);
        auto const uniqueEnd = std::ranges::unique(expectedHashes).begin();
        expectedHashes.erase(uniqueEnd, expectedHashes.end());
        if (expectedHashes.size() != encodedTemplates.size())
        {
            return invalidRuntime(
                std::format(
                    "runtime template closure requires {} unique assets but received {}",
                    expectedHashes.size(),
                    encodedTemplates.size()
                )
            );
        }

        auto templates = std::vector<GrayTemplate>{};
        templates.reserve(encodedTemplates.size());
        for (auto index = std::size_t{0}; index < encodedTemplates.size(); ++index)
        {
            auto& encoded       = encodedTemplates[index];
            auto const expected = expectedHashes[index];
            if (encoded.hash != expected)
            {
                return invalidRuntime(
                    std::format(
                        "runtime template closure expected {} but received {}",
                        expected.toString(),
                        encoded.hash.toString()
                    )
                );
            }

            UF_TRY_VALUE(actualHash, sha256(encoded.pngBytes));
            if (actualHash != encoded.hash)
            {
                return invalidRuntime(
                    std::format(
                        "runtime template {} does not match its content hash",
                        encoded.hash.toString()
                    )
                );
            }

            UF_TRY_VALUE(
                decoded,
                image::decodePng(
                    encoded.pngBytes,
                    encoded.hash.toString()
                )
            );
            auto const width  = decoded.width;
            auto const height = decoded.height;
            UF_TRY_VALUE(bgra, image::rgba8ToBgra8(std::move(decoded.pixels)));
            auto const widthSize = checkedCast<std::size_t>(width);
            if (!widthSize)
            {
                return invalidRuntime("runtime template width is not addressable");
            }
            auto const stride = checkedMultiply(*widthSize, std::size_t{4});
            if (!stride)
            {
                return invalidRuntime("runtime template stride overflowed");
            }
            UF_TRY_VALUE(
                gray,
                bgra8ToGray8(bgra, width, height, *stride)
            );
            UF_TRY_VALUE(
                alpha,
                bgra8ToAlpha8(bgra, width, height, *stride)
            );
            // A template that excludes nothing keeps an empty mask, so it takes
            // the same matcher call it took before templates carried one.
            auto const opaque = std::ranges::all_of(
                alpha,
                [](std::byte weight) noexcept -> bool
                {
                    return weight == std::byte{255};
                }
            );
            templates.emplace_back(
                GrayTemplate{
                    .hash   = encoded.hash,
                    .width  = width,
                    .height = height,
                    .pixels = std::move(gray),
                    .mask   = opaque ? std::vector<std::byte>{} : std::move(alpha),
                }
            );
        }

        auto runtime = RecognitionRuntime{
            std::move(manifest),
            std::move(templates)
        };
        for (auto const& recognizer : runtime.m_manifest.catalog().recognizers())
        {
            for (auto const& variant : recognizer.variants())
            {
                auto const* p_asset = runtime.m_manifest.findAsset(
                    recognizer.id(),
                    variant.name
                );
                UF_CHECK(p_asset != nullptr);
                auto const* p_template = runtime.findTemplate(p_asset->templateHash);
                UF_CHECK(p_template != nullptr);
                if (
                    p_template->width != variant.templateRect.width()
                    || p_template->height != variant.templateRect.height()
                )
                {
                    return invalidRuntime(
                        std::format(
                            "runtime template {} dimensions {}x{} do not match variant {} geometry {}x{}",
                            p_asset->templateHash.toString(),
                            p_template->width,
                            p_template->height,
                            variant.name.value(),
                            variant.templateRect.width(),
                            variant.templateRect.height()
                        )
                    );
                }
            }
        }
        return runtime;
    }

    auto RecognitionRuntime::findTemplate(
        ContentHash const& hash
    ) const noexcept -> GrayTemplate const*
    {
        auto const found = std::ranges::lower_bound(
            m_templates,
            hash,
            {},
            &GrayTemplate::hash
        );
        if (found == m_templates.end() || found->hash != hash)
        {
            return nullptr;
        }
        return &*found;
    }

    auto RecognitionRuntime::manifest() const noexcept -> RuntimeManifest const&
    {
        return m_manifest;
    }

    auto RecognitionRuntime::matchGrayTemplate(
        GrayImage const& grayFrame,
        GrayTemplate const& grayTemplate,
        PixelRect roi,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) -> Result<SadSearchReport>
    {
        auto const templateStride = checkedCast<std::size_t>(grayTemplate.width);
        UF_CHECK(templateStride.has_value());
        UF_TRY_VALUE(
            templateImage,
            GrayImage::create(
                grayTemplate.pixels,
                grayTemplate.width,
                grayTemplate.height,
                *templateStride
            )
        );
        if (grayTemplate.mask.empty())
        {
            return matchTemplateSad(
                grayFrame,
                templateImage,
                roi,
                maximumPixelComparisons,
                poll
            );
        }

        UF_TRY_VALUE(
            maskImage,
            GrayImage::create(
                grayTemplate.mask,
                grayTemplate.width,
                grayTemplate.height,
                *templateStride
            )
        );
        return matchTemplateSad(
            grayFrame,
            templateImage,
            maskImage,
            roi,
            maximumPixelComparisons,
            poll
        );
    }

    auto RecognitionRuntime::evaluateGrayPage(
        Frame const& frame,
        GrayImage const& grayFrame,
        RecognitionPolicy const& policy,
        SadSearchPoll const& poll
    ) const -> Result<PageRecognitionAttempt>
    {
        auto evaluations               = std::vector<AnchorEvaluation>{};
        auto completedEvidence         = std::vector<AnchorEvidence>{};
        auto completedPixelComparisons = uint64{0};
        auto const& catalog            = m_manifest.catalog();
        auto const anchorOrder         = catalog.pageAnchorOrder();
        evaluations.reserve(anchorOrder.size());
        completedEvidence.reserve(anchorOrder.size());

        for (auto const id : anchorOrder)
        {
            auto const* p_recognizer = catalog.findRecognizer(id);
            UF_CHECK(p_recognizer != nullptr);

            auto const remainingBudget = checkedSubtract(
                policy.maximumPixelComparisons,
                completedPixelComparisons
            );
            UF_CHECK(remainingBudget.has_value());
            // No page is known yet -- that is exactly what lets one search
            // serve every page -- so identify never honours a pinned variant
            // and always folds across all of them.
            UF_TRY_VALUE(
                attempt,
                matchElement(
                    grayFrame,
                    *p_recognizer,
                    p_recognizer->searchRoi(),
                    std::nullopt,
                    *remainingBudget,
                    poll
                )
            );
            auto const newCompleted = checkedAdd(
                completedPixelComparisons,
                attempt.completedPixelComparisons
            );
            UF_CHECK(newCompleted.has_value());
            completedPixelComparisons = *newCompleted;

            if (
                auto const* p_stop = std::get_if<SadSearchStopReason>(
                    &attempt.result
                )
            )
            {
                return PageRecognitionAttempt{
                    .result = PageRecognitionStop{
                        .recognizerId = id,
                        .reason       = *p_stop,
                    },
                    .completedAnchorEvidence   = std::move(completedEvidence),
                    .completedPixelComparisons = completedPixelComparisons,
                };
            }

            auto const& evidence = std::get<AnchorEvidence>(attempt.result);
            completedEvidence.emplace_back(evidence);
            evaluations.emplace_back(
                AnchorEvaluation::fromEvidence(evidence)
            );
        }

        UF_TRY_VALUE(
            pageOutcome,
            PageResolver::resolve(
                catalog,
                FrameIdentity::fromFrame(frame),
                evaluations
            )
        );
        return PageRecognitionAttempt{
            .result                    = std::move(pageOutcome),
            .completedAnchorEvidence   = std::move(completedEvidence),
            .completedPixelComparisons = completedPixelComparisons,
        };
    }

    auto RecognitionRuntime::ensureCompatibleFrame(
        Frame const& frame,
        ProjectFingerprint liveFingerprint
    ) const -> Status
    {
        auto const expected = m_manifest.catalog().fingerprint();
        if (liveFingerprint != expected)
        {
            return incompatibleFrame(
                std::format(
                    "live fingerprint {}x{} @ {}x{} DPI does not match project {}x{} @ {}x{} DPI",
                    liveFingerprint.width(),
                    liveFingerprint.height(),
                    liveFingerprint.dpiX(),
                    liveFingerprint.dpiY(),
                    expected.width(),
                    expected.height(),
                    expected.dpiX(),
                    expected.dpiY()
                )
            );
        }
        if (frame.width() != expected.width() || frame.height() != expected.height())
        {
            return incompatibleFrame(
                std::format(
                    "frame extent {}x{} does not match project {}x{}",
                    frame.width(),
                    frame.height(),
                    expected.width(),
                    expected.height()
                )
            );
        }

        return ok();
    }

    auto RecognitionRuntime::evaluatePage(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        RecognitionPolicy const& policy
    ) const -> Result<PageRecognitionAttempt>
    {
        UF_TRY(ensureCompatibleFrame(frame, liveFingerprint));

        auto const poll = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [this, &frame, &policy, &poll](
                GrayImage const& grayFrame
            ) -> Result<PageRecognitionAttempt>
            {
                return evaluateGrayPage(frame, grayFrame, policy, poll);
            }
        );
    }

    auto RecognitionRuntime::recognizePage(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        RecognitionPolicy const& policy
    ) const -> Result<PageOutcome>
    {
        UF_TRY_VALUE(attempt, evaluatePage(frame, liveFingerprint, policy));
        if (
            auto const* p_stop = std::get_if<PageRecognitionStop>(
                &attempt.result
            )
        )
        {
            return pageRecognitionFailure(*p_stop);
        }
        return std::get<PageOutcome>(std::move(attempt.result));
    }

    auto RecognitionRuntime::matchElement(
        GrayImage const& grayFrame,
        RecognizerDefinition const& recognizer,
        PixelRect searchRoi,
        std::optional<ResourceName> const& pinnedVariant,
        uint64 maximumPixelComparisons,
        SadSearchPoll const& poll
    ) const -> Result<ElementMatchAttempt>
    {
        auto completedPixelComparisons = uint64{0};
        auto best                      = std::optional<AnchorEvidence>{};
        for (auto const& variant : recognizer.variants())
        {
            if (pinnedVariant.has_value() && variant.name != *pinnedVariant)
            {
                continue;
            }

            auto const* p_asset = m_manifest.findAsset(recognizer.id(), variant.name);
            UF_CHECK(p_asset != nullptr);
            auto const* p_template = findTemplate(p_asset->templateHash);
            UF_CHECK(p_template != nullptr);

            auto const remainingBudget = checkedSubtract(
                maximumPixelComparisons,
                completedPixelComparisons
            );
            UF_CHECK(remainingBudget.has_value());
            UF_TRY_VALUE(
                sadReport,
                matchGrayTemplate(
                    grayFrame,
                    *p_template,
                    searchRoi,
                    *remainingBudget,
                    poll
                )
            );
            auto const newCompleted = checkedAdd(
                completedPixelComparisons,
                sadReport.completedPixelComparisons
            );
            UF_CHECK(newCompleted.has_value());
            completedPixelComparisons = *newCompleted;

            if (
                auto const* p_stop = std::get_if<SadSearchStopReason>(
                    &sadReport.outcome
                )
            )
            {
                return ElementMatchAttempt{
                    .result                    = *p_stop,
                    .completedPixelComparisons = completedPixelComparisons,
                };
            }

            UF_TRY_VALUE(
                evaluation,
                AnchorEvaluation::fromSadOutcome(
                    recognizer,
                    variant,
                    searchRoi,
                    sadReport.outcome
                )
            );
            auto const* p_evidence = std::get_if<AnchorEvidence>(
                &evaluation.evaluation()
            );
            UF_CHECK(p_evidence != nullptr);
            if (!best)
            {
                best = *p_evidence;
                continue;
            }
            UF_TRY_VALUE(better, strictlyBetter(*p_evidence, *best));
            if (better)
            {
                best = *p_evidence;
            }
        }

        UF_CHECK_MSG(
            best.has_value(),
            "matching an element requires at least one searchable variant"
        );
        return ElementMatchAttempt{
            .result                    = *std::move(best),
            .completedPixelComparisons = completedPixelComparisons,
        };
    }

    auto RecognitionRuntime::evaluateActionTarget(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        PageId pageId,
        ElementId elementId,
        RecognitionPolicy const& policy
    ) const -> Result<ActionTargetAttempt>
    {
        UF_TRY(ensureCompatibleFrame(frame, liveFingerprint));

        auto const& catalog      = m_manifest.catalog();
        auto const* p_recognizer = catalog.findRecognizer(elementId);
        if (p_recognizer == nullptr)
        {
            return invalidRuntime(
                std::format(
                    "element {} is not present in the runtime catalog",
                    elementId.value().toString()
                )
            );
        }

        // Authorisation IS the reference, so a page that does not exercise
        // interact on this element has no action here to locate.
        auto const* p_reference = catalog.findReference(pageId, elementId);
        if (p_reference == nullptr || !p_reference->exercised.hasInteract())
        {
            return invalidRuntime(
                "the resolved page does not exercise interact on this element"
            );
        }

        auto const searchRoi = p_reference->searchRoi.value_or(
            p_recognizer->searchRoi()
        );

        // No variants means the page's own resolution located it: the
        // rectangle is where it was annotated, and there is nothing to match.
        if (p_recognizer->variants().empty())
        {
            return ActionTargetAttempt{
                .result                    = AnchorEvidence::locatedByPage(elementId, searchRoi),
                .completedPixelComparisons = uint64{0},
            };
        }

        auto const poll = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [this, p_recognizer, p_reference, searchRoi, &policy, &poll](
                GrayImage const& grayFrame
            ) -> Result<ActionTargetAttempt>
            {
                UF_TRY_VALUE(
                    attempt,
                    matchElement(
                        grayFrame,
                        *p_recognizer,
                        searchRoi,
                        p_reference->variant,
                        policy.maximumPixelComparisons,
                        poll
                    )
                );
                if (
                    auto const* p_stop = std::get_if<SadSearchStopReason>(
                        &attempt.result
                    )
                )
                {
                    return ActionTargetAttempt{
                        .result = PageRecognitionStop{
                            .recognizerId = p_recognizer->id(),
                            .reason       = *p_stop,
                        },
                        .completedPixelComparisons = attempt.completedPixelComparisons,
                    };
                }
                return ActionTargetAttempt{
                    .result                    = std::get<AnchorEvidence>(std::move(attempt.result)),
                    .completedPixelComparisons = attempt.completedPixelComparisons,
                };
            }
        );
    }

    auto resolveClickPixel(
        RecognizerDefinition const& recognizer,
        PixelRect const& matchedRect
    ) -> Result<PixelPoint>
    {
        auto const& interact = recognizer.capabilities().interact();
        if (!interact.has_value())
        {
            return invalidRuntime(
                "only an element that interacts defines a click point"
            );
        }

        if (auto const offset = interact->clickOffset)
        {
            auto const clickX = checkedAdd(matchedRect.x(), offset->x());
            auto const clickY = checkedAdd(matchedRect.y(), offset->y());
            if (!clickX || !clickY)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "action_target click offset overflowed the matched rectangle"
                );
            }
            return PixelPoint{*clickX, *clickY};
        }

        // Integer division truncates toward zero, so the center resolves to one
        // reproducible pixel for both even and odd rectangle extents. The origin
        // plus half the extent cannot exceed the validated right/bottom edge, so
        // the sum stays inside uint32 without a checked add.
        auto const centerX = matchedRect.x() + matchedRect.width() / 2U;
        auto const centerY = matchedRect.y() + matchedRect.height() / 2U;
        return PixelPoint{centerX, centerY};
    }
}
