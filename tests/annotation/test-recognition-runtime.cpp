#include "test-helpers.hpp"

#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/runtime-manifest.hpp>

#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>
#include <domain/frame.hpp>

#include <image/png.hpp>

#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_anchorAId = "00000000-0000-0000-0000-000000000011";
        constexpr auto k_anchorBId = "00000000-0000-0000-0000-000000000012";
        constexpr auto k_actionId  = "00000000-0000-0000-0000-000000000013";
        constexpr auto k_pageAId   = "00000000-0000-0000-0000-000000000111";
        constexpr auto k_pageBId   = "00000000-0000-0000-0000-000000000112";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        [[nodiscard]]
        auto encodedTemplate(
            uint8 gray,
            uint32 width = 1
        ) -> EncodedRuntimeTemplate
        {
            auto const widthSize = checkedCast<std::size_t>(width);
            REQUIRE(widthSize.has_value());
            auto const rgbaSize = checkedMultiply(
                widthSize.value_or(std::size_t{0}),
                std::size_t{4}
            );
            REQUIRE(rgbaSize.has_value());
            auto rgba = std::vector<std::byte>{};
            rgba.reserve(rgbaSize.value_or(std::size_t{0}));
            for (auto x = uint32{0}; x < width; ++x)
            {
                rgba.emplace_back(asByte(gray));
                rgba.emplace_back(asByte(gray));
                rgba.emplace_back(asByte(gray));
                rgba.emplace_back(asByte(255));
            }
            auto encoded = image::encodeRgbaPng(
                "recognition-runtime-template.png",
                width,
                1,
                rgba
            );
            REQUIRE(encoded.has_value());
            auto const hash = sha256(*encoded);
            REQUIRE(hash.has_value());
            return EncodedRuntimeTemplate{
                .hash     = *hash,
                .pngBytes = *std::move(encoded),
            };
        }

        enum class RuntimeTemplateLayout : uint8
        {
            Distinct,
            Shared,
            MismatchedWidth,
        };

        struct RuntimeInput final
        {
            RuntimeManifest                     manifest;
            std::vector<EncodedRuntimeTemplate> templates{};
            ProjectFingerprint                  fingerprint;

            RecognizerId anchorA;
            RecognizerId anchorB;
            PageId       pageA;
            PageId       pageB;
        };

        [[nodiscard]]
        auto runtimeInput(
            RuntimeTemplateLayout layout = RuntimeTemplateLayout::Distinct
        ) -> RuntimeInput
        {
            auto const fingerprint    = test::fingerprint(3, 1, 96, 96);
            auto const anchorA        = test::recognizerId(k_anchorAId);
            auto const anchorB        = test::recognizerId(k_anchorBId);
            auto const pageA          = test::pageId(k_pageAId);
            auto const pageB          = test::pageId(k_pageBId);
            auto const templateAWidth = (
                layout == RuntimeTemplateLayout::MismatchedWidth ? 2U : 1U
            );
            auto templateA = encodedTemplate(2, templateAWidth);
            auto templateB = (
                layout == RuntimeTemplateLayout::Shared
                    ? templateA
                    : encodedTemplate(3)
            );
            auto const sourceBytes = std::array{asByte(42)};
            auto const sourceHash  = sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto manifest = RuntimeManifest::create(
                test::projectId("personal.recognition_runtime"),
                fingerprint,
                {
                    RuntimeRecognizerSpec{
                        .definition = test::recognizer(
                            fingerprint,
                            anchorA,
                            "anchor_a",
                            AnnotationType::PageAnchor,
                            test::pixelRect(0, 0, 1, 1),
                            test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            test::threshold(10'000)
                        ),
                        .templateHash = templateA.hash,
                        .sourceHash   = *sourceHash,
                    },
                    RuntimeRecognizerSpec{
                        .definition = test::recognizer(
                            fingerprint,
                            anchorB,
                            "anchor_b",
                            AnnotationType::PageAnchor,
                            test::pixelRect(0, 0, 1, 1),
                            test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            test::threshold(10'000)
                        ),
                        .templateHash = templateB.hash,
                        .sourceHash   = *sourceHash,
                    },
                },
                {
                    test::page(pageA, "page_a", {anchorA}, {anchorB}),
                    test::page(pageB, "page_b", {anchorA}),
                }
            );
            REQUIRE(manifest.has_value());
            auto templates = std::vector<EncodedRuntimeTemplate>{};
            if (layout == RuntimeTemplateLayout::Shared)
            {
                templates.emplace_back(std::move(templateA));
            }
            else
            {
                templates.emplace_back(std::move(templateB));
                templates.emplace_back(std::move(templateA));
            }
            return RuntimeInput{
                .manifest    = *std::move(manifest),
                .templates   = std::move(templates),
                .fingerprint = fingerprint,
                .anchorA     = anchorA,
                .anchorB     = anchorB,
                .pageA       = pageA,
                .pageB       = pageB,
            };
        }

        struct RuntimeFixture final
        {
            RecognitionRuntime runtime;
            ProjectFingerprint fingerprint{test::fingerprint()};
            RecognizerId       anchorA{test::recognizerId(k_anchorAId)};
            RecognizerId       anchorB{test::recognizerId(k_anchorBId)};
            PageId             pageA{test::pageId(k_pageAId)};
            PageId             pageB{test::pageId(k_pageBId)};
        };

        [[nodiscard]]
        auto runtimeFixture() -> RuntimeFixture
        {
            auto input = runtimeInput();
            auto runtime = RecognitionRuntime::create(
                std::move(input.manifest),
                std::move(input.templates)
            );
            REQUIRE(runtime.has_value());
            return RuntimeFixture{
                .runtime     = *std::move(runtime),
                .fingerprint = input.fingerprint,
                .anchorA     = input.anchorA,
                .anchorB     = input.anchorB,
                .pageA       = input.pageA,
                .pageB       = input.pageB,
            };
        }

        [[nodiscard]]
        auto continuingPolicy(uint64 budget) -> RecognitionPolicy
        {
            return RecognitionPolicy{
                .maximumPixelComparisons = budget,
            };
        }

        [[nodiscard]]
        auto runtimeFrame(
            ProjectFingerprint fingerprint,
            std::vector<std::byte> pixels,
            PixelFormat pixelFormat,
            FrameId frameId = FrameId{17}
        ) -> Frame
        {
            auto const transform = CoordinateTransform::create(
                Point<DesktopSpace>{0.0F, 0.0F},
                static_cast<float>(fingerprint.width()),
                static_cast<float>(fingerprint.height()),
                fingerprint.width(),
                fingerprint.height()
            );
            REQUIRE(transform.has_value());
            auto const width = checkedCast<std::size_t>(fingerprint.width());
            REQUIRE(width.has_value());
            auto const stride = checkedMultiply(
                width.value_or(std::size_t{0}),
                bytesPerPixel(pixelFormat)
            );
            REQUIRE(stride.has_value());
            auto const buffer = std::shared_ptr<FrameBuffer const>{
                std::make_shared<FrameBuffer>(std::move(pixels))
            };
            auto frame = Frame::create(
                frameId,
                CaptureSessionId{7},
                TargetGeneration::fromValue(3),
                MonotonicInstant::fromTimePoint(MonotonicInstant::TimePoint{}),
                fingerprint.width(),
                fingerprint.height(),
                stride.value_or(std::size_t{0}),
                pixelFormat,
                buffer,
                *transform
            );
            REQUIRE(frame.has_value());
            return *std::move(frame);
        }

        struct TemplatePixel final
        {
            uint8 gray{};
            uint8 alpha{};
        };

        [[nodiscard]]
        auto encodedAlphaTemplate(
            std::span<TemplatePixel const> pixels
        ) -> EncodedRuntimeTemplate
        {
            auto const width = checkedCast<uint32>(pixels.size());
            REQUIRE(width.has_value());
            auto rgba = std::vector<std::byte>{};
            rgba.reserve(pixels.size() * 4U);
            for (auto const pixel : pixels)
            {
                rgba.emplace_back(asByte(pixel.gray));
                rgba.emplace_back(asByte(pixel.gray));
                rgba.emplace_back(asByte(pixel.gray));
                rgba.emplace_back(asByte(pixel.alpha));
            }
            auto encoded = image::encodeRgbaPng(
                "recognition-runtime-alpha-template.png",
                *width,
                1,
                rgba
            );
            REQUIRE(encoded.has_value());
            auto const hash = sha256(*encoded);
            REQUIRE(hash.has_value());
            return EncodedRuntimeTemplate{
                .hash     = *hash,
                .pngBytes = *std::move(encoded),
            };
        }

        struct ActionRuntimeInput final
        {
            RuntimeManifest                     manifest;
            std::vector<EncodedRuntimeTemplate> templates{};
            ProjectFingerprint                  fingerprint;

            RecognizerId anchorA;
            RecognizerId actionTarget;
            PageId       pageA;
        };

        [[nodiscard]]
        auto actionRuntimeInput() -> ActionRuntimeInput
        {
            auto const fingerprint = test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = test::recognizerId(k_anchorAId);
            auto const actionT     = test::recognizerId(k_actionId);
            auto const pageA       = test::pageId(k_pageAId);
            auto anchorTemplate = encodedTemplate(2);
            auto actionTemplate = encodedTemplate(5);
            auto const sourceBytes = std::array{asByte(42)};
            auto const sourceHash  = sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto manifest = RuntimeManifest::create(
                test::projectId("personal.action_target_runtime"),
                fingerprint,
                {
                    RuntimeRecognizerSpec{
                        .definition = test::recognizer(
                            fingerprint,
                            anchorA,
                            "anchor_a",
                            AnnotationType::PageAnchor,
                            test::pixelRect(0, 0, 1, 1),
                            test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            test::threshold(10'000)
                        ),
                        .templateHash = anchorTemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                    RuntimeRecognizerSpec{
                        .definition = test::recognizer(
                            fingerprint,
                            actionT,
                            "action_target",
                            AnnotationType::ActionTarget,
                            test::pixelRect(0, 0, 1, 1),
                            test::pixelRect(0, 0, 3, 1),
                            {pageA},
                            std::nullopt,
                            test::threshold(10'000)
                        ),
                        .templateHash = actionTemplate.hash,
                        .sourceHash   = *sourceHash,
                    },
                },
                {
                    test::page(pageA, "page_a", {anchorA}),
                }
            );
            REQUIRE(manifest.has_value());
            auto templates = std::vector<EncodedRuntimeTemplate>{};
            templates.emplace_back(std::move(anchorTemplate));
            templates.emplace_back(std::move(actionTemplate));
            return ActionRuntimeInput{
                .manifest     = *std::move(manifest),
                .templates    = std::move(templates),
                .fingerprint  = fingerprint,
                .anchorA      = anchorA,
                .actionTarget = actionT,
                .pageA        = pageA,
            };
        }

        struct ActionFixture final
        {
            RecognitionRuntime runtime;
            ProjectFingerprint fingerprint{test::fingerprint(3, 1, 96, 96)};
            RecognizerId       anchorA{test::recognizerId(k_anchorAId)};
            RecognizerId       actionTarget{test::recognizerId(k_actionId)};
            PageId             pageA{test::pageId(k_pageAId)};
        };

        [[nodiscard]]
        auto actionFixture() -> ActionFixture
        {
            auto input             = actionRuntimeInput();
            auto const fingerprint = input.fingerprint;
            auto const anchorA     = input.anchorA;
            auto const actionT     = input.actionTarget;
            auto const pageA       = input.pageA;
            auto runtime = RecognitionRuntime::create(
                std::move(input.manifest),
                std::move(input.templates)
            );
            REQUIRE(runtime.has_value());
            return ActionFixture{
                .runtime      = *std::move(runtime),
                .fingerprint  = fingerprint,
                .anchorA      = anchorA,
                .actionTarget = actionT,
                .pageA        = pageA,
            };
        }
    }

    TEST_CASE("page recognition shares exact Gray8 and colored BGRA8 evidence")
    {
        auto const fixture = runtimeFixture();
        auto const policy  = continuingPolicy(100);
        auto const grayPixels = std::vector{
            asByte(1),
            asByte(2),
            asByte(1),
        };
        auto const colorPixels = std::vector{
            asByte(0), asByte(0), asByte(4), asByte(255),
            asByte(0), asByte(4), asByte(0), asByte(255),
            asByte(9), asByte(0), asByte(0), asByte(255),
        };
        auto const gray = runtimeFrame(
            fixture.fingerprint,
            grayPixels,
            PixelFormat::Gray8
        );
        auto const color = runtimeFrame(
            fixture.fingerprint,
            colorPixels,
            PixelFormat::Bgra8
        );

        auto const grayAttempt = fixture.runtime.evaluatePage(
            gray,
            fixture.fingerprint,
            policy
        );
        auto const colorAttempt = fixture.runtime.evaluatePage(
            color,
            fixture.fingerprint,
            policy
        );
        REQUIRE(grayAttempt.has_value());
        REQUIRE(colorAttempt.has_value());
        CHECK(
            grayAttempt->completedAnchorEvidence
            == colorAttempt->completedAnchorEvidence
        );
        CHECK(
            grayAttempt->completedPixelComparisons
            == colorAttempt->completedPixelComparisons
        );
        CHECK(grayAttempt->completedPixelComparisons == 5);

        auto const* p_grayPage  = std::get_if<PageOutcome>(&grayAttempt->result);
        auto const* p_colorPage = std::get_if<PageOutcome>(&colorAttempt->result);
        REQUIRE(p_grayPage != nullptr);
        REQUIRE(p_colorPage != nullptr);
        REQUIRE(std::holds_alternative<AmbiguousPages>(*p_grayPage));
        REQUIRE(std::holds_alternative<AmbiguousPages>(*p_colorPage));
        auto const grayCandidates = std::get<AmbiguousPages>(
            *p_grayPage
        ).evidence().candidatePageIds();
        auto const colorCandidates = std::get<AmbiguousPages>(
            *p_colorPage
        ).evidence().candidatePageIds();
        REQUIRE(grayCandidates.size() == 2U);
        REQUIRE(colorCandidates.size() == grayCandidates.size());
        CHECK(grayCandidates[0] == fixture.pageA);
        CHECK(grayCandidates[1] == fixture.pageB);
        CHECK(colorCandidates[0] == grayCandidates[0]);
        CHECK(colorCandidates[1] == grayCandidates[1]);
    }

    TEST_CASE("recognition runtime resolves and rejects pages without heuristics")
    {
        auto const fixture = runtimeFixture();
        auto const policy  = continuingPolicy(100);
        auto const resolvedFrame = runtimeFrame(
            fixture.fingerprint,
            {asByte(2), asByte(3), asByte(1)},
            PixelFormat::Gray8
        );
        auto const unknownFrame = runtimeFrame(
            fixture.fingerprint,
            {asByte(0), asByte(0), asByte(0)},
            PixelFormat::Gray8,
            FrameId{18}
        );

        auto const resolved = fixture.runtime.evaluatePage(
            resolvedFrame,
            fixture.fingerprint,
            policy
        );
        auto const unknown = fixture.runtime.evaluatePage(
            unknownFrame,
            fixture.fingerprint,
            policy
        );
        REQUIRE(resolved.has_value());
        REQUIRE(unknown.has_value());
        auto const* p_resolvedPage = std::get_if<PageOutcome>(&resolved->result);
        auto const* p_unknownPage  = std::get_if<PageOutcome>(&unknown->result);
        REQUIRE(p_resolvedPage != nullptr);
        REQUIRE(p_unknownPage != nullptr);
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_resolvedPage));
        REQUIRE(std::holds_alternative<UnknownPage>(*p_unknownPage));
        CHECK(std::get<ResolvedPage>(*p_resolvedPage).pageId() == fixture.pageB);
        CHECK(resolved->completedPixelComparisons == 3);
        CHECK(unknown->completedPixelComparisons == 6);

        auto const operationalResolved = fixture.runtime.recognizePage(
            resolvedFrame,
            fixture.fingerprint,
            policy
        );
        auto const operationalUnknown = fixture.runtime.recognizePage(
            unknownFrame,
            fixture.fingerprint,
            policy
        );
        REQUIRE(operationalResolved.has_value());
        REQUIRE(operationalUnknown.has_value());
        REQUIRE(std::holds_alternative<ResolvedPage>(*operationalResolved));
        REQUIRE(std::holds_alternative<UnknownPage>(*operationalUnknown));
    }

    TEST_CASE("recognition runtime stops the complete page evaluation on global budget")
    {
        auto const fixture = runtimeFixture();
        auto const policy  = continuingPolicy(2);
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.runtime.evaluatePage(
            frame,
            fixture.fingerprint,
            policy
        );
        REQUIRE(attempt.has_value());
        auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt->result);
        REQUIRE(p_stop != nullptr);
        CHECK(p_stop->recognizerId == fixture.anchorB);
        CHECK(p_stop->reason == SadSearchStopReason::ComparisonBudgetExhausted);
        REQUIRE(attempt->completedAnchorEvidence.size() == 1U);
        CHECK(attempt->completedAnchorEvidence.front().recognizerId() == fixture.anchorA);
        CHECK(attempt->completedPixelComparisons == 2);

        auto const outcome = fixture.runtime.recognizePage(
            frame,
            fixture.fingerprint,
            policy
        );
        REQUIRE_FALSE(outcome.has_value());
        test::requireErrorKind(
            outcome.error(),
            AutomationErrorKind::RecognitionIncomplete
        );
        // The page evaluation stopped before it reached anchorB, so the run knows
        // nothing about that anchor; the caller has to observe again rather than
        // treat the page as ruled out.
        CHECK(failureResponse(outcome.error()) == FailureResponse::Retry);
    }

    TEST_CASE("recognition runtime preserves an immediate cancellation as a stop")
    {
        auto const fixture    = runtimeFixture();
        auto cancellation     = std::stop_source{};
        auto const didRequest = cancellation.request_stop();
        auto const policy = RecognitionPolicy{
            .maximumPixelComparisons = 100,
            .cancellation            = cancellation.get_token(),
        };
        REQUIRE(didRequest);
        auto const frame = runtimeFrame(
            fixture.fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.runtime.evaluatePage(
            frame,
            fixture.fingerprint,
            policy
        );
        REQUIRE(attempt.has_value());
        auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt->result);
        REQUIRE(p_stop != nullptr);
        CHECK(p_stop->recognizerId == fixture.anchorA);
        CHECK(p_stop->reason == SadSearchStopReason::Cancelled);
        CHECK(attempt->completedAnchorEvidence.empty());
        CHECK(attempt->completedPixelComparisons == 0);

        auto const outcome = fixture.runtime.recognizePage(
            frame,
            fixture.fingerprint,
            policy
        );
        REQUIRE_FALSE(outcome.has_value());
        test::requireErrorKind(
            outcome.error(),
            AutomationErrorKind::Cancelled
        );
    }

    TEST_CASE("recognition runtime preserves an expired deadline as a timeout")
    {
        auto const fixture = runtimeFixture();
        auto const policy = RecognitionPolicy{
            .maximumPixelComparisons = 100,
            .deadline = MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{}
            ),
        };
        auto const frame = runtimeFrame(
            fixture.fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.runtime.evaluatePage(
            frame,
            fixture.fingerprint,
            policy
        );
        REQUIRE(attempt.has_value());
        auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt->result);
        REQUIRE(p_stop != nullptr);
        CHECK(p_stop->recognizerId == fixture.anchorA);
        CHECK(p_stop->reason == SadSearchStopReason::TimedOut);
        CHECK(attempt->completedAnchorEvidence.empty());
        CHECK(attempt->completedPixelComparisons == 0);

        auto const outcome = fixture.runtime.recognizePage(
            frame,
            fixture.fingerprint,
            policy
        );
        REQUIRE_FALSE(outcome.has_value());
        test::requireErrorKind(
            outcome.error(),
            AutomationErrorKind::Timeout
        );
    }

    TEST_CASE("recognition runtime rejects incompatible frames and template closure damage")
    {
        auto const fixture = runtimeFixture();
        auto const policy  = continuingPolicy(100);
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );
        auto const incompatible = fixture.runtime.recognizePage(
            frame,
            test::fingerprint(3, 1, 120, 120),
            policy
        );
        REQUIRE_FALSE(incompatible.has_value());
        test::requireErrorKind(
            incompatible.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );

        auto missing = runtimeInput();
        missing.templates.pop_back();
        auto const missingRuntime = RecognitionRuntime::create(
            std::move(missing.manifest),
            std::move(missing.templates)
        );
        REQUIRE_FALSE(missingRuntime.has_value());
        test::requireErrorKind(
            missingRuntime.error(),
            AutomationErrorKind::InvalidResource
        );

        auto corrupt = runtimeInput();
        REQUIRE_FALSE(corrupt.templates.front().pngBytes.empty());
        corrupt.templates.front().pngBytes.back() ^= std::byte{1};
        auto const corruptRuntime = RecognitionRuntime::create(
            std::move(corrupt.manifest),
            std::move(corrupt.templates)
        );
        REQUIRE_FALSE(corruptRuntime.has_value());
        test::requireErrorKind(
            corruptRuntime.error(),
            AutomationErrorKind::InvalidResource
        );

        auto mismatched = runtimeInput(RuntimeTemplateLayout::MismatchedWidth);
        auto const mismatchedRuntime = RecognitionRuntime::create(
            std::move(mismatched.manifest),
            std::move(mismatched.templates)
        );
        REQUIRE_FALSE(mismatchedRuntime.has_value());
        test::requireErrorKind(
            mismatchedRuntime.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("recognition runtime accepts one encoded asset shared by multiple anchors")
    {
        auto input             = runtimeInput(RuntimeTemplateLayout::Shared);
        auto const fingerprint = input.fingerprint;
        auto const pageB       = input.pageB;
        CHECK(input.templates.size() == 1U);
        auto runtime = RecognitionRuntime::create(
            std::move(input.manifest),
            std::move(input.templates)
        );
        REQUIRE(runtime.has_value());
        auto const frame = runtimeFrame(
            fingerprint,
            {asByte(2), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const outcome = runtime->recognizePage(
            frame,
            fingerprint,
            continuingPolicy(100)
        );
        REQUIRE(outcome.has_value());
        REQUIRE(std::holds_alternative<ResolvedPage>(*outcome));
        CHECK(std::get<ResolvedPage>(*outcome).pageId() == pageB);
    }

    TEST_CASE("recognition runtime evaluates an action target hit into match evidence")
    {
        auto const fixture = actionFixture();
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.runtime.evaluateActionTarget(
            frame,
            fixture.fingerprint,
            fixture.actionTarget,
            continuingPolicy(100)
        );
        REQUIRE(attempt.has_value());
        auto const* p_evidence = std::get_if<AnchorEvidence>(&attempt->result);
        REQUIRE(p_evidence != nullptr);
        CHECK(p_evidence->recognizerId() == fixture.actionTarget);
        CHECK(p_evidence->hit());
        REQUIRE(p_evidence->sadScore().has_value());
        CHECK(p_evidence->sadScore().value() == 0);
        CHECK(p_evidence->sadScore().value() <= p_evidence->maximumSad());
        REQUIRE(p_evidence->matchedRect().has_value());
        CHECK(p_evidence->matchedRect().value() == test::pixelRect(1, 0, 1, 1));
        CHECK(attempt->completedPixelComparisons == 2);
    }

    TEST_CASE("recognition runtime reports an absent action target as tier a absence")
    {
        auto const fixture = actionFixture();
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(0), asByte(0), asByte(0)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.runtime.evaluateActionTarget(
            frame,
            fixture.fingerprint,
            fixture.actionTarget,
            continuingPolicy(100)
        );
        REQUIRE(attempt.has_value());
        auto const* p_evidence = std::get_if<AnchorEvidence>(&attempt->result);
        REQUIRE(p_evidence != nullptr);
        CHECK(p_evidence->recognizerId() == fixture.actionTarget);
        CHECK_FALSE(p_evidence->hit());
        // The reusable matcher always returns its best candidate while the
        // recognizer search_roi fits its template, so a miss is Tier A absence
        // carrying a best score above threshold rather than an error.
        REQUIRE(p_evidence->sadScore().has_value());
        CHECK(p_evidence->sadScore().value() > p_evidence->maximumSad());
    }

    TEST_CASE("recognition runtime rejects recognizers that are not catalog action targets")
    {
        auto const fixture = actionFixture();
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto const unknown = fixture.runtime.evaluateActionTarget(
            frame,
            fixture.fingerprint,
            test::recognizerId(k_anchorBId),
            continuingPolicy(100)
        );
        REQUIRE_FALSE(unknown.has_value());
        test::requireErrorKind(unknown.error(), AutomationErrorKind::InvalidResource);
        CHECK(
            unknown.error().message().find("not present")
            != std::string_view::npos
        );

        auto const wrongType = fixture.runtime.evaluateActionTarget(
            frame,
            fixture.fingerprint,
            fixture.anchorA,
            continuingPolicy(100)
        );
        REQUIRE_FALSE(wrongType.has_value());
        test::requireErrorKind(wrongType.error(), AutomationErrorKind::InvalidResource);
        CHECK(
            wrongType.error().message().find("action_target")
            != std::string_view::npos
        );
    }

    TEST_CASE("recognition runtime rejects an action target on an incompatible fingerprint")
    {
        auto const fixture = actionFixture();
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto const incompatible = fixture.runtime.evaluateActionTarget(
            frame,
            test::fingerprint(3, 1, 120, 120),
            fixture.actionTarget,
            continuingPolicy(100)
        );
        REQUIRE_FALSE(incompatible.has_value());
        test::requireErrorKind(
            incompatible.error(),
            AutomationErrorKind::TargetCompatibilityUnverified
        );
    }

    TEST_CASE("recognition runtime preserves action target control stops")
    {
        auto const fixture = actionFixture();
        auto const frame   = runtimeFrame(
            fixture.fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto cancellation     = std::stop_source{};
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);
        auto const cancelled = fixture.runtime.evaluateActionTarget(
            frame,
            fixture.fingerprint,
            fixture.actionTarget,
            RecognitionPolicy{
                .maximumPixelComparisons = 100,
                .cancellation            = cancellation.get_token(),
            }
        );
        REQUIRE(cancelled.has_value());
        auto const* p_cancelStop = std::get_if<PageRecognitionStop>(
            &cancelled->result
        );
        REQUIRE(p_cancelStop != nullptr);
        CHECK(p_cancelStop->recognizerId == fixture.actionTarget);
        CHECK(p_cancelStop->reason == SadSearchStopReason::Cancelled);
        CHECK(cancelled->completedPixelComparisons == 0);

        auto const exhausted = fixture.runtime.evaluateActionTarget(
            frame,
            fixture.fingerprint,
            fixture.actionTarget,
            continuingPolicy(0)
        );
        REQUIRE(exhausted.has_value());
        auto const* p_budgetStop = std::get_if<PageRecognitionStop>(
            &exhausted->result
        );
        REQUIRE(p_budgetStop != nullptr);
        CHECK(p_budgetStop->recognizerId == fixture.actionTarget);
        CHECK(
            p_budgetStop->reason
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(exhausted->completedPixelComparisons == 0);
    }

    TEST_CASE("a template's alpha channel excludes its pixels from recognition")
    {
        auto const fingerprint = test::fingerprint(3, 1, 96, 96);
        auto const maskedId    = test::recognizerId(k_anchorAId);
        auto const opaqueId    = test::recognizerId(k_anchorBId);
        auto const pageA       = test::pageId(k_pageAId);

        // The same two pixels twice: the second one is artwork the first
        // template excludes through its alpha channel and the second keeps.
        auto const maskedPixels = std::array{
            TemplatePixel{2, 255},
            TemplatePixel{200, 0},
        };
        auto const opaquePixels = std::array{
            TemplatePixel{2, 255},
            TemplatePixel{200, 255},
        };
        auto maskedTemplate = encodedAlphaTemplate(maskedPixels);
        auto opaqueTemplate = encodedAlphaTemplate(opaquePixels);

        auto const sourceBytes = std::array{asByte(42)};
        auto const sourceHash  = sha256(sourceBytes);
        REQUIRE(sourceHash.has_value());

        auto manifest = RuntimeManifest::create(
            test::projectId("personal.masked_runtime"),
            fingerprint,
            {
                RuntimeRecognizerSpec{
                    .definition = test::recognizer(
                        fingerprint,
                        maskedId,
                        "masked_anchor",
                        AnnotationType::PageAnchor,
                        test::pixelRect(0, 0, 2, 1),
                        test::pixelRect(0, 0, 3, 1),
                        {},
                        std::nullopt,
                        test::threshold(10'000)
                    ),
                    .templateHash = maskedTemplate.hash,
                    .sourceHash   = *sourceHash,
                },
                RuntimeRecognizerSpec{
                    .definition = test::recognizer(
                        fingerprint,
                        opaqueId,
                        "opaque_anchor",
                        AnnotationType::PageAnchor,
                        test::pixelRect(0, 0, 2, 1),
                        test::pixelRect(0, 0, 3, 1),
                        {},
                        std::nullopt,
                        test::threshold(10'000)
                    ),
                    .templateHash = opaqueTemplate.hash,
                    .sourceHash   = *sourceHash,
                },
            },
            {
                test::page(pageA, "page_a", {maskedId, opaqueId}),
            }
        );
        REQUIRE(manifest.has_value());
        auto templates = std::vector<EncodedRuntimeTemplate>{};
        templates.emplace_back(std::move(maskedTemplate));
        templates.emplace_back(std::move(opaqueTemplate));
        auto runtime = RecognitionRuntime::create(
            *std::move(manifest),
            std::move(templates)
        );
        REQUIRE(runtime.has_value());

        // The frame keeps the first template pixel and replaces the artwork
        // behind the second one.
        auto const frame = runtimeFrame(
            fingerprint,
            {asByte(2), asByte(99), asByte(250)},
            PixelFormat::Gray8
        );
        auto const attempt = runtime->evaluatePage(
            frame,
            fingerprint,
            continuingPolicy(100)
        );
        REQUIRE(attempt.has_value());

        auto const evidenceFor = [&attempt](RecognizerId id) -> AnchorEvidence
        {
            auto const found = std::ranges::find(
                attempt->completedAnchorEvidence,
                id,
                &AnchorEvidence::recognizerId
            );
            REQUIRE(found != attempt->completedAnchorEvidence.end());
            return *found;
        };

        auto const masked = evidenceFor(maskedId);
        CHECK(masked.hit());
        REQUIRE(masked.sadScore().has_value());
        CHECK(masked.sadScore().value() == 0);
        REQUIRE(masked.matchedRect().has_value());
        CHECK(masked.matchedRect().value() == test::pixelRect(0, 0, 2, 1));

        // The identical template without the alpha hole sees the artwork and
        // misses under the same exact threshold.
        auto const opaque = evidenceFor(opaqueId);
        CHECK_FALSE(opaque.hit());
        REQUIRE(opaque.sadScore().has_value());
        CHECK(opaque.sadScore().value() == 101);

        // The excluded pixel is still walked, so the budget is unchanged: the
        // masked anchor exits on its exact hit and the opaque one prunes.
        CHECK(attempt->completedPixelComparisons == 6);
    }

    TEST_CASE("resolveClickPixel derives deterministic integer click points")
    {
        auto const fingerprint = test::fingerprint(8, 8, 96, 96);
        auto const actionT     = test::recognizerId(k_actionId);
        auto const pageA       = test::pageId(k_pageAId);

        auto const offset = TemplateOffset::create(2, 1, 4, 3);
        REQUIRE(offset.has_value());
        auto const withOffset = test::recognizer(
            fingerprint,
            actionT,
            "action_target",
            AnnotationType::ActionTarget,
            test::pixelRect(0, 0, 4, 3),
            test::pixelRect(0, 0, 4, 3),
            {pageA},
            *offset,
            test::threshold(10'000)
        );
        auto const offsetClick = resolveClickPixel(
            withOffset,
            test::pixelRect(10, 20, 4, 3)
        );
        REQUIRE(offsetClick.has_value());
        CHECK(*offsetClick == PixelPoint{12, 21});

        // Odd extents pin the truncating integer center: 3 / 2 == 1, 5 / 2 == 2.
        auto const centered = test::recognizer(
            fingerprint,
            actionT,
            "action_target",
            AnnotationType::ActionTarget,
            test::pixelRect(0, 0, 3, 5),
            test::pixelRect(0, 0, 3, 5),
            {pageA},
            std::nullopt,
            test::threshold(10'000)
        );
        auto const centerClick = resolveClickPixel(
            centered,
            test::pixelRect(2, 4, 3, 5)
        );
        REQUIRE(centerClick.has_value());
        CHECK(*centerClick == PixelPoint{3, 6});

        auto const anchor = test::recognizer(
            fingerprint,
            test::recognizerId(k_anchorAId),
            "anchor_a",
            AnnotationType::PageAnchor,
            test::pixelRect(0, 0, 3, 5),
            test::pixelRect(0, 0, 3, 5),
            {},
            std::nullopt,
            test::threshold(10'000)
        );
        auto const rejected = resolveClickPixel(
            anchor,
            test::pixelRect(2, 4, 3, 5)
        );
        REQUIRE_FALSE(rejected.has_value());
        test::requireErrorKind(rejected.error(), AutomationErrorKind::InvalidResource);
        CHECK(
            rejected.error().message().find("action_target")
            != std::string_view::npos
        );
    }
}
