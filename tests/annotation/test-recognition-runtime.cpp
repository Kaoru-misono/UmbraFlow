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

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto g_anchorAId = "00000000-0000-0000-0000-000000000011";
        constexpr auto g_anchorBId = "00000000-0000-0000-0000-000000000012";
        constexpr auto g_actionId  = "00000000-0000-0000-0000-000000000013";
        constexpr auto g_pageAId   = "00000000-0000-0000-0000-000000000111";
        constexpr auto g_pageBId   = "00000000-0000-0000-0000-000000000112";

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
                .m_hash     = *hash,
                .m_pngBytes = *std::move(encoded),
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
            RuntimeManifest                     m_manifest;
            std::vector<EncodedRuntimeTemplate> m_templates{};
            ProjectFingerprint                  m_fingerprint;

            RecognizerId m_anchorA;
            RecognizerId m_anchorB;
            PageId       m_pageA;
            PageId       m_pageB;
        };

        [[nodiscard]]
        auto runtimeInput(
            RuntimeTemplateLayout layout = RuntimeTemplateLayout::Distinct
        ) -> RuntimeInput
        {
            auto const fingerprint    = test::fingerprint(3, 1, 96, 96);
            auto const anchorA        = test::recognizerId(g_anchorAId);
            auto const anchorB        = test::recognizerId(g_anchorBId);
            auto const pageA          = test::pageId(g_pageAId);
            auto const pageB          = test::pageId(g_pageBId);
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
                        .m_definition = test::recognizer(
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
                        .m_templateHash = templateA.m_hash,
                        .m_sourceHash   = *sourceHash,
                    },
                    RuntimeRecognizerSpec{
                        .m_definition = test::recognizer(
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
                        .m_templateHash = templateB.m_hash,
                        .m_sourceHash   = *sourceHash,
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
                .m_manifest    = *std::move(manifest),
                .m_templates   = std::move(templates),
                .m_fingerprint = fingerprint,
                .m_anchorA     = anchorA,
                .m_anchorB     = anchorB,
                .m_pageA       = pageA,
                .m_pageB       = pageB,
            };
        }

        struct RuntimeFixture final
        {
            RecognitionRuntime m_runtime;
            ProjectFingerprint m_fingerprint{test::fingerprint()};
            RecognizerId       m_anchorA{test::recognizerId(g_anchorAId)};
            RecognizerId       m_anchorB{test::recognizerId(g_anchorBId)};
            PageId             m_pageA{test::pageId(g_pageAId)};
            PageId             m_pageB{test::pageId(g_pageBId)};
        };

        [[nodiscard]]
        auto runtimeFixture() -> RuntimeFixture
        {
            auto input = runtimeInput();
            auto runtime = RecognitionRuntime::create(
                std::move(input.m_manifest),
                std::move(input.m_templates)
            );
            REQUIRE(runtime.has_value());
            return RuntimeFixture{
                .m_runtime     = *std::move(runtime),
                .m_fingerprint = input.m_fingerprint,
                .m_anchorA     = input.m_anchorA,
                .m_anchorB     = input.m_anchorB,
                .m_pageA       = input.m_pageA,
                .m_pageB       = input.m_pageB,
            };
        }

        [[nodiscard]]
        auto continuingPolicy(uint64 budget) -> RecognitionPolicy
        {
            return RecognitionPolicy{
                .m_maximumPixelComparisons = budget,
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
                SessionId{7},
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

        struct ActionRuntimeInput final
        {
            RuntimeManifest                     m_manifest;
            std::vector<EncodedRuntimeTemplate> m_templates{};
            ProjectFingerprint                  m_fingerprint;

            RecognizerId m_anchorA;
            RecognizerId m_actionTarget;
            PageId       m_pageA;
        };

        [[nodiscard]]
        auto actionRuntimeInput() -> ActionRuntimeInput
        {
            auto const fingerprint = test::fingerprint(3, 1, 96, 96);
            auto const anchorA     = test::recognizerId(g_anchorAId);
            auto const actionT     = test::recognizerId(g_actionId);
            auto const pageA       = test::pageId(g_pageAId);
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
                        .m_definition = test::recognizer(
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
                        .m_templateHash = anchorTemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
                    },
                    RuntimeRecognizerSpec{
                        .m_definition = test::recognizer(
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
                        .m_templateHash = actionTemplate.m_hash,
                        .m_sourceHash   = *sourceHash,
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
                .m_manifest     = *std::move(manifest),
                .m_templates    = std::move(templates),
                .m_fingerprint  = fingerprint,
                .m_anchorA      = anchorA,
                .m_actionTarget = actionT,
                .m_pageA        = pageA,
            };
        }

        struct ActionFixture final
        {
            RecognitionRuntime m_runtime;
            ProjectFingerprint m_fingerprint{test::fingerprint(3, 1, 96, 96)};
            RecognizerId       m_anchorA{test::recognizerId(g_anchorAId)};
            RecognizerId       m_actionTarget{test::recognizerId(g_actionId)};
            PageId             m_pageA{test::pageId(g_pageAId)};
        };

        [[nodiscard]]
        auto actionFixture() -> ActionFixture
        {
            auto input             = actionRuntimeInput();
            auto const fingerprint = input.m_fingerprint;
            auto const anchorA     = input.m_anchorA;
            auto const actionT     = input.m_actionTarget;
            auto const pageA       = input.m_pageA;
            auto runtime = RecognitionRuntime::create(
                std::move(input.m_manifest),
                std::move(input.m_templates)
            );
            REQUIRE(runtime.has_value());
            return ActionFixture{
                .m_runtime      = *std::move(runtime),
                .m_fingerprint  = fingerprint,
                .m_anchorA      = anchorA,
                .m_actionTarget = actionT,
                .m_pageA        = pageA,
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
            fixture.m_fingerprint,
            grayPixels,
            PixelFormat::Gray8
        );
        auto const color = runtimeFrame(
            fixture.m_fingerprint,
            colorPixels,
            PixelFormat::Bgra8
        );

        auto const grayAttempt = fixture.m_runtime.evaluatePage(
            gray,
            fixture.m_fingerprint,
            policy
        );
        auto const colorAttempt = fixture.m_runtime.evaluatePage(
            color,
            fixture.m_fingerprint,
            policy
        );
        REQUIRE(grayAttempt.has_value());
        REQUIRE(colorAttempt.has_value());
        CHECK(
            grayAttempt->m_completedAnchorEvidence
            == colorAttempt->m_completedAnchorEvidence
        );
        CHECK(
            grayAttempt->m_completedPixelComparisons
            == colorAttempt->m_completedPixelComparisons
        );
        CHECK(grayAttempt->m_completedPixelComparisons == 5);

        auto const* p_grayPage  = std::get_if<PageOutcome>(&grayAttempt->m_result);
        auto const* p_colorPage = std::get_if<PageOutcome>(&colorAttempt->m_result);
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
        CHECK(grayCandidates[0] == fixture.m_pageA);
        CHECK(grayCandidates[1] == fixture.m_pageB);
        CHECK(colorCandidates[0] == grayCandidates[0]);
        CHECK(colorCandidates[1] == grayCandidates[1]);
    }

    TEST_CASE("recognition runtime resolves and rejects pages without heuristics")
    {
        auto const fixture = runtimeFixture();
        auto const policy  = continuingPolicy(100);
        auto const resolvedFrame = runtimeFrame(
            fixture.m_fingerprint,
            {asByte(2), asByte(3), asByte(1)},
            PixelFormat::Gray8
        );
        auto const unknownFrame = runtimeFrame(
            fixture.m_fingerprint,
            {asByte(0), asByte(0), asByte(0)},
            PixelFormat::Gray8,
            FrameId{18}
        );

        auto const resolved = fixture.m_runtime.evaluatePage(
            resolvedFrame,
            fixture.m_fingerprint,
            policy
        );
        auto const unknown = fixture.m_runtime.evaluatePage(
            unknownFrame,
            fixture.m_fingerprint,
            policy
        );
        REQUIRE(resolved.has_value());
        REQUIRE(unknown.has_value());
        auto const* p_resolvedPage = std::get_if<PageOutcome>(&resolved->m_result);
        auto const* p_unknownPage  = std::get_if<PageOutcome>(&unknown->m_result);
        REQUIRE(p_resolvedPage != nullptr);
        REQUIRE(p_unknownPage != nullptr);
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_resolvedPage));
        REQUIRE(std::holds_alternative<UnknownPage>(*p_unknownPage));
        CHECK(std::get<ResolvedPage>(*p_resolvedPage).pageId() == fixture.m_pageB);
        CHECK(resolved->m_completedPixelComparisons == 3);
        CHECK(unknown->m_completedPixelComparisons == 6);

        auto const operationalResolved = fixture.m_runtime.recognizePage(
            resolvedFrame,
            fixture.m_fingerprint,
            policy
        );
        auto const operationalUnknown = fixture.m_runtime.recognizePage(
            unknownFrame,
            fixture.m_fingerprint,
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
            fixture.m_fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.m_runtime.evaluatePage(
            frame,
            fixture.m_fingerprint,
            policy
        );
        REQUIRE(attempt.has_value());
        auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt->m_result);
        REQUIRE(p_stop != nullptr);
        CHECK(p_stop->m_recognizerId == fixture.m_anchorB);
        CHECK(p_stop->m_reason == SadSearchStopReason::ComparisonBudgetExhausted);
        REQUIRE(attempt->m_completedAnchorEvidence.size() == 1U);
        CHECK(attempt->m_completedAnchorEvidence.front().recognizerId() == fixture.m_anchorA);
        CHECK(attempt->m_completedPixelComparisons == 2);

        auto const outcome = fixture.m_runtime.recognizePage(
            frame,
            fixture.m_fingerprint,
            policy
        );
        REQUIRE_FALSE(outcome.has_value());
        test::requireErrorKind(
            outcome.error(),
            AutomationErrorKind::RecognitionFailed
        );
    }

    TEST_CASE("recognition runtime preserves an immediate cancellation as a stop")
    {
        auto const fixture    = runtimeFixture();
        auto cancellation     = std::stop_source{};
        auto const didRequest = cancellation.request_stop();
        auto const policy = RecognitionPolicy{
            .m_maximumPixelComparisons = 100,
            .m_cancellation            = cancellation.get_token(),
        };
        REQUIRE(didRequest);
        auto const frame = runtimeFrame(
            fixture.m_fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.m_runtime.evaluatePage(
            frame,
            fixture.m_fingerprint,
            policy
        );
        REQUIRE(attempt.has_value());
        auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt->m_result);
        REQUIRE(p_stop != nullptr);
        CHECK(p_stop->m_recognizerId == fixture.m_anchorA);
        CHECK(p_stop->m_reason == SadSearchStopReason::Cancelled);
        CHECK(attempt->m_completedAnchorEvidence.empty());
        CHECK(attempt->m_completedPixelComparisons == 0);

        auto const outcome = fixture.m_runtime.recognizePage(
            frame,
            fixture.m_fingerprint,
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
            .m_maximumPixelComparisons = 100,
            .m_deadline = MonotonicInstant::fromTimePoint(
                MonotonicInstant::TimePoint{}
            ),
        };
        auto const frame = runtimeFrame(
            fixture.m_fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.m_runtime.evaluatePage(
            frame,
            fixture.m_fingerprint,
            policy
        );
        REQUIRE(attempt.has_value());
        auto const* p_stop = std::get_if<PageRecognitionStop>(&attempt->m_result);
        REQUIRE(p_stop != nullptr);
        CHECK(p_stop->m_recognizerId == fixture.m_anchorA);
        CHECK(p_stop->m_reason == SadSearchStopReason::TimedOut);
        CHECK(attempt->m_completedAnchorEvidence.empty());
        CHECK(attempt->m_completedPixelComparisons == 0);

        auto const outcome = fixture.m_runtime.recognizePage(
            frame,
            fixture.m_fingerprint,
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
            fixture.m_fingerprint,
            {asByte(1), asByte(2), asByte(1)},
            PixelFormat::Gray8
        );
        auto const incompatible = fixture.m_runtime.recognizePage(
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
        missing.m_templates.pop_back();
        auto const missingRuntime = RecognitionRuntime::create(
            std::move(missing.m_manifest),
            std::move(missing.m_templates)
        );
        REQUIRE_FALSE(missingRuntime.has_value());
        test::requireErrorKind(
            missingRuntime.error(),
            AutomationErrorKind::InvalidResource
        );

        auto corrupt = runtimeInput();
        REQUIRE_FALSE(corrupt.m_templates.front().m_pngBytes.empty());
        corrupt.m_templates.front().m_pngBytes.back() ^= std::byte{1};
        auto const corruptRuntime = RecognitionRuntime::create(
            std::move(corrupt.m_manifest),
            std::move(corrupt.m_templates)
        );
        REQUIRE_FALSE(corruptRuntime.has_value());
        test::requireErrorKind(
            corruptRuntime.error(),
            AutomationErrorKind::InvalidResource
        );

        auto mismatched = runtimeInput(RuntimeTemplateLayout::MismatchedWidth);
        auto const mismatchedRuntime = RecognitionRuntime::create(
            std::move(mismatched.m_manifest),
            std::move(mismatched.m_templates)
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
        auto const fingerprint = input.m_fingerprint;
        auto const pageB       = input.m_pageB;
        CHECK(input.m_templates.size() == 1U);
        auto runtime = RecognitionRuntime::create(
            std::move(input.m_manifest),
            std::move(input.m_templates)
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
            fixture.m_fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.m_runtime.evaluateActionTarget(
            frame,
            fixture.m_fingerprint,
            fixture.m_actionTarget,
            continuingPolicy(100)
        );
        REQUIRE(attempt.has_value());
        auto const* p_evidence = std::get_if<AnchorEvidence>(&attempt->m_result);
        REQUIRE(p_evidence != nullptr);
        CHECK(p_evidence->recognizerId() == fixture.m_actionTarget);
        CHECK(p_evidence->hit());
        REQUIRE(p_evidence->sadScore().has_value());
        CHECK(p_evidence->sadScore().value() == 0);
        CHECK(p_evidence->sadScore().value() <= p_evidence->maximumSad());
        REQUIRE(p_evidence->matchedRect().has_value());
        CHECK(p_evidence->matchedRect().value() == test::pixelRect(1, 0, 1, 1));
        CHECK(attempt->m_completedPixelComparisons == 2);
    }

    TEST_CASE("recognition runtime reports an absent action target as tier a absence")
    {
        auto const fixture = actionFixture();
        auto const frame   = runtimeFrame(
            fixture.m_fingerprint,
            {asByte(0), asByte(0), asByte(0)},
            PixelFormat::Gray8
        );

        auto const attempt = fixture.m_runtime.evaluateActionTarget(
            frame,
            fixture.m_fingerprint,
            fixture.m_actionTarget,
            continuingPolicy(100)
        );
        REQUIRE(attempt.has_value());
        auto const* p_evidence = std::get_if<AnchorEvidence>(&attempt->m_result);
        REQUIRE(p_evidence != nullptr);
        CHECK(p_evidence->recognizerId() == fixture.m_actionTarget);
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
            fixture.m_fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto const unknown = fixture.m_runtime.evaluateActionTarget(
            frame,
            fixture.m_fingerprint,
            test::recognizerId(g_anchorBId),
            continuingPolicy(100)
        );
        REQUIRE_FALSE(unknown.has_value());
        test::requireErrorKind(unknown.error(), AutomationErrorKind::InvalidResource);
        CHECK(
            unknown.error().message().find("not present")
            != std::string_view::npos
        );

        auto const wrongType = fixture.m_runtime.evaluateActionTarget(
            frame,
            fixture.m_fingerprint,
            fixture.m_anchorA,
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
            fixture.m_fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto const incompatible = fixture.m_runtime.evaluateActionTarget(
            frame,
            test::fingerprint(3, 1, 120, 120),
            fixture.m_actionTarget,
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
            fixture.m_fingerprint,
            {asByte(0), asByte(5), asByte(0)},
            PixelFormat::Gray8
        );

        auto cancellation     = std::stop_source{};
        auto const didRequest = cancellation.request_stop();
        REQUIRE(didRequest);
        auto const cancelled = fixture.m_runtime.evaluateActionTarget(
            frame,
            fixture.m_fingerprint,
            fixture.m_actionTarget,
            RecognitionPolicy{
                .m_maximumPixelComparisons = 100,
                .m_cancellation            = cancellation.get_token(),
            }
        );
        REQUIRE(cancelled.has_value());
        auto const* p_cancelStop = std::get_if<PageRecognitionStop>(
            &cancelled->m_result
        );
        REQUIRE(p_cancelStop != nullptr);
        CHECK(p_cancelStop->m_recognizerId == fixture.m_actionTarget);
        CHECK(p_cancelStop->m_reason == SadSearchStopReason::Cancelled);
        CHECK(cancelled->m_completedPixelComparisons == 0);

        auto const exhausted = fixture.m_runtime.evaluateActionTarget(
            frame,
            fixture.m_fingerprint,
            fixture.m_actionTarget,
            continuingPolicy(0)
        );
        REQUIRE(exhausted.has_value());
        auto const* p_budgetStop = std::get_if<PageRecognitionStop>(
            &exhausted->m_result
        );
        REQUIRE(p_budgetStop != nullptr);
        CHECK(p_budgetStop->m_recognizerId == fixture.m_actionTarget);
        CHECK(
            p_budgetStop->m_reason
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
        CHECK(exhausted->m_completedPixelComparisons == 0);
    }

    TEST_CASE("resolveClickPixel derives deterministic integer click points")
    {
        auto const fingerprint = test::fingerprint(8, 8, 96, 96);
        auto const actionT     = test::recognizerId(g_actionId);
        auto const pageA       = test::pageId(g_pageAId);

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
            test::recognizerId(g_anchorAId),
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
