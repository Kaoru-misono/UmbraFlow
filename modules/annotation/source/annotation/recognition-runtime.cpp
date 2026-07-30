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
            auto const* p_asset = runtime.m_manifest.findAsset(recognizer.id());
            UF_CHECK(p_asset != nullptr);
            auto const* p_template = runtime.findTemplate(p_asset->templateHash);
            UF_CHECK(p_template != nullptr);
            auto const templateRect = recognizer.templateRect();
            if (
                p_template->width != templateRect.width()
                || p_template->height != templateRect.height()
            )
            {
                return invalidRuntime(
                    std::format(
                        "runtime template {} dimensions {}x{} do not match recognizer {} geometry {}x{}",
                        p_asset->templateHash.toString(),
                        p_template->width,
                        p_template->height,
                        recognizer.id().value().toString(),
                        templateRect.width(),
                        templateRect.height()
                    )
                );
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
            auto const* p_asset      = m_manifest.findAsset(id);
            UF_CHECK(p_recognizer != nullptr);
            UF_CHECK(p_asset != nullptr);
            auto const* p_template = findTemplate(p_asset->templateHash);
            UF_CHECK(p_template != nullptr);

            auto const remainingBudget = checkedSubtract(
                policy.maximumPixelComparisons,
                completedPixelComparisons
            );
            UF_CHECK(remainingBudget.has_value());
            UF_TRY_VALUE(
                sadReport,
                matchGrayTemplate(
                    grayFrame,
                    *p_template,
                    p_recognizer->searchRoi(),
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
                return PageRecognitionAttempt{
                    .result = PageRecognitionStop{
                        .recognizerId = id,
                        .reason       = *p_stop,
                    },
                    .completedAnchorEvidence   = std::move(completedEvidence),
                    .completedPixelComparisons = completedPixelComparisons,
                };
            }

            UF_TRY_VALUE(
                evaluation,
                AnchorEvaluation::fromSadOutcome(
                    *p_recognizer,
                    sadReport.outcome
                )
            );
            auto const* p_evidence = std::get_if<AnchorEvidence>(
                &evaluation.evaluation()
            );
            UF_CHECK(p_evidence != nullptr);
            completedEvidence.emplace_back(*p_evidence);
            evaluations.emplace_back(evaluation);
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

    auto RecognitionRuntime::evaluateGrayActionTarget(
        GrayImage const& grayFrame,
        RecognizerDefinition const& recognizer,
        GrayTemplate const& grayTemplate,
        RecognitionPolicy const& policy,
        SadSearchPoll const& poll
    ) const -> Result<ActionTargetAttempt>
    {
        UF_TRY_VALUE(
            sadReport,
            matchGrayTemplate(
                grayFrame,
                grayTemplate,
                recognizer.searchRoi(),
                policy.maximumPixelComparisons,
                poll
            )
        );

        if (
            auto const* p_stop = std::get_if<SadSearchStopReason>(
                &sadReport.outcome
            )
        )
        {
            return ActionTargetAttempt{
                .result = PageRecognitionStop{
                    .recognizerId = recognizer.id(),
                    .reason       = *p_stop,
                },
                .completedPixelComparisons = sadReport.completedPixelComparisons,
            };
        }

        UF_TRY_VALUE(
            evaluation,
            AnchorEvaluation::fromSadOutcome(recognizer, sadReport.outcome)
        );
        auto const* p_evidence = std::get_if<AnchorEvidence>(
            &evaluation.evaluation()
        );
        UF_CHECK(p_evidence != nullptr);
        return ActionTargetAttempt{
            .result                    = *p_evidence,
            .completedPixelComparisons = sadReport.completedPixelComparisons,
        };
    }

    auto RecognitionRuntime::evaluateActionTarget(
        Frame const& frame,
        ProjectFingerprint liveFingerprint,
        ElementId recognizerId,
        RecognitionPolicy const& policy
    ) const -> Result<ActionTargetAttempt>
    {
        UF_TRY(ensureCompatibleFrame(frame, liveFingerprint));

        auto const& catalog      = m_manifest.catalog();
        auto const* p_recognizer = catalog.findRecognizer(recognizerId);
        if (p_recognizer == nullptr)
        {
            return invalidRuntime(
                std::format(
                    "recognizer {} is not present in the runtime catalog",
                    recognizerId.value().toString()
                )
            );
        }
        if (p_recognizer->annotationType() != AnnotationType::ActionTarget)
        {
            return invalidRuntime(
                "only an action_target may be evaluated for an action"
            );
        }

        auto const* p_asset = m_manifest.findAsset(recognizerId);
        UF_CHECK(p_asset != nullptr);
        auto const* p_template = findTemplate(p_asset->templateHash);
        UF_CHECK(p_template != nullptr);

        auto const poll = makeSadSearchPoll(policy);
        return withGrayFrame(
            frame,
            [this, p_recognizer, p_template, &policy, &poll](
                GrayImage const& grayFrame
            ) -> Result<ActionTargetAttempt>
            {
                return evaluateGrayActionTarget(
                    grayFrame,
                    *p_recognizer,
                    *p_template,
                    policy,
                    poll
                );
            }
        );
    }

    auto resolveClickPixel(
        RecognizerDefinition const& recognizer,
        PixelRect const& matchedRect
    ) -> Result<PixelPoint>
    {
        if (recognizer.annotationType() != AnnotationType::ActionTarget)
        {
            return invalidRuntime(
                "only an action_target defines a click point"
            );
        }

        if (auto const offset = recognizer.defaultClick())
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
