#include "test-helpers.hpp"

#include <annotation/catalog.hpp>
#include <annotation/recognition.hpp>

#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <vision/sad.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_anchorAId = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_anchorBId = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_pageAId = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_pageBId = "00000000-0000-0000-0000-000000000102";

        auto anchorEvaluation(
            RecognizerDefinition const& recognizer,
            uint64 score
        ) -> AnchorEvaluation
        {
            auto const outcome = SadSearchOutcome{
                std::optional<SadMatch>{SadMatch{0, 0, score}}
            };
            auto result = AnchorEvaluation::fromSadOutcome(recognizer, outcome);
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        struct ResolutionFixture final
        {
            RecognitionCatalog catalog;
            RecognizerId anchorA{test::recognizerId(k_anchorAId)};
            RecognizerId anchorB{test::recognizerId(k_anchorBId)};
            PageId pageA{test::pageId(k_pageAId)};
            PageId pageB{test::pageId(k_pageBId)};
        };

        auto resolutionFixture() -> ResolutionFixture
        {
            auto const projectFingerprint = test::fingerprint();
            auto const anchorA = test::recognizerId(k_anchorAId);
            auto const anchorB = test::recognizerId(k_anchorBId);
            auto const pageA = test::pageId(k_pageAId);
            auto const pageB = test::pageId(k_pageBId);
            auto recognizers = std::vector<RecognizerDefinition>{};
            recognizers.emplace_back(
                test::recognizer(
                    projectFingerprint,
                    anchorA,
                    "anchor_a",
                    AnnotationType::PageAnchor,
                    test::pixelRect(0, 0, 1, 1),
                    test::pixelRect(0, 0, 4, 4)
                )
            );
            recognizers.emplace_back(
                test::recognizer(
                    projectFingerprint,
                    anchorB,
                    "anchor_b",
                    AnnotationType::PageAnchor,
                    test::pixelRect(0, 0, 1, 1),
                    test::pixelRect(0, 0, 4, 4)
                )
            );
            auto pages = std::vector<PageSignature>{};
            pages.emplace_back(test::page(pageA, "page_a", {anchorA}, {anchorB}));
            pages.emplace_back(test::page(pageB, "page_b", {anchorA}));
            return ResolutionFixture{
                .catalog = test::catalog(
                    projectFingerprint,
                    std::move(recognizers),
                    std::move(pages)
                ),
                .anchorA = anchorA,
                .anchorB = anchorB,
                .pageA   = pageA,
                .pageB   = pageB,
            };
        }

        auto resolve(
            ResolutionFixture const& fixture,
            uint64 anchorAScore,
            uint64 anchorBScore
        ) -> Result<PageOutcome>
        {
            auto const* p_anchorA = fixture.catalog.findRecognizer(fixture.anchorA);
            auto const* p_anchorB = fixture.catalog.findRecognizer(fixture.anchorB);
            REQUIRE(p_anchorA != nullptr);
            REQUIRE(p_anchorB != nullptr);
            auto const evaluations = std::array{
                anchorEvaluation(*p_anchorA, anchorAScore),
                anchorEvaluation(*p_anchorB, anchorBScore),
            };
            return PageResolver::resolve(
                fixture.catalog,
                FrameIdentity{CaptureSessionId{7}, TargetGeneration::fromValue(3), FrameId{11}},
                evaluations
            );
        }
    }

    TEST_CASE("anchor evidence accepts the inclusive integer SAD boundary")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchor = test::recognizer(
            projectFingerprint,
            test::recognizerId(k_anchorAId),
            "anchor",
            AnnotationType::PageAnchor,
            test::pixelRect(0, 0, 2, 2),
            test::pixelRect(0, 0, 4, 4)
        );
        struct BoundaryCase final
        {
            uint64 score{};
            bool hit{};
        };
        auto const cases = std::array{
            BoundaryCase{101, true},
            BoundaryCase{102, true},
            BoundaryCase{103, false},
        };
        for (auto const testCase : cases)
        {
            auto const evaluation = anchorEvaluation(anchor, testCase.score);
            auto const* p_evidence = std::get_if<AnchorEvidence>(
                &evaluation.evaluation()
            );
            REQUIRE(p_evidence != nullptr);
            CHECK(p_evidence->maximumSad() == 102);
            CHECK(p_evidence->hit() == testCase.hit);
            CHECK(p_evidence->sadScore() == testCase.score);
            REQUIRE(p_evidence->matchedRect().has_value());
        }
    }

    TEST_CASE("page resolution returns resolved unknown and ambiguous without priority")
    {
        auto const fixture = resolutionFixture();

        auto const unknown = resolve(fixture, 26, 26);
        REQUIRE(unknown.has_value());
        REQUIRE(std::holds_alternative<UnknownPage>(*unknown));
        CHECK(
            std::get<UnknownPage>(*unknown).evidence().candidatePageIds().empty()
        );

        auto const resolved = resolve(fixture, 0, 0);
        REQUIRE(resolved.has_value());
        REQUIRE(std::holds_alternative<ResolvedPage>(*resolved));
        auto const& page = std::get<ResolvedPage>(*resolved);
        CHECK(page.pageId() == fixture.pageB);
        CHECK(page.evidence().pages().size() == 2);

        auto const ambiguous = resolve(fixture, 0, 26);
        REQUIRE(ambiguous.has_value());
        REQUIRE(std::holds_alternative<AmbiguousPages>(*ambiguous));
        auto const candidates = std::get<AmbiguousPages>(
            *ambiguous
        ).evidence().candidatePageIds();
        REQUIRE(candidates.size() == 2);
        CHECK(candidates[0] == fixture.pageA);
        CHECK(candidates[1] == fixture.pageB);
    }

    TEST_CASE("a search that never finished is not classified as a decided miss")
    {
        auto const fixture = resolutionFixture();
        auto const* p_anchor = fixture.catalog.findRecognizer(fixture.anchorA);
        REQUIRE(p_anchor != nullptr);
        auto const identity = FrameIdentity{
            CaptureSessionId{7},
            TargetGeneration::fromValue(3),
            FrameId{11}
        };

        struct StopCase final
        {
            SadSearchStopReason reason{};
            AutomationErrorKind expectedKind{};
            FailureResponse     expectedResponse{};
        };

        // Each of the three reasons carries its own kind, and none of the three
        // lands on StepFailed, which is the response a caller reads as "this step
        // is ruled out, take the other branch". A stop rules nothing out.
        auto const cases = std::array{
            StopCase{
                SadSearchStopReason::Cancelled,
                AutomationErrorKind::Cancelled,
                FailureResponse::Cancelled,
            },
            StopCase{
                SadSearchStopReason::TimedOut,
                AutomationErrorKind::Timeout,
                FailureResponse::Abort,
            },
            StopCase{
                SadSearchStopReason::ComparisonBudgetExhausted,
                AutomationErrorKind::RecognitionIncomplete,
                FailureResponse::Retry,
            },
        };
        for (auto const testCase : cases)
        {
            CHECK(searchStopKind(testCase.reason) == testCase.expectedKind);
            CHECK(
                failureResponse(searchStopKind(testCase.reason))
                == testCase.expectedResponse
            );
            CHECK(
                failureResponse(searchStopKind(testCase.reason))
                != FailureResponse::StepFailed
            );

            auto const sadOutcome = SadSearchOutcome{testCase.reason};
            auto const evaluation = AnchorEvaluation::fromSadOutcome(
                *p_anchor,
                sadOutcome
            );
            REQUIRE(evaluation.has_value());
            auto const evaluations = std::array{*evaluation};
            auto const result = PageResolver::resolve(
                fixture.catalog,
                identity,
                evaluations
            );
            REQUIRE_FALSE(result.has_value());
            test::requireErrorKind(result.error(), testCase.expectedKind);
        }

        // The other half, without which the checks above would pass on a matcher
        // that never decided anything: a search that did complete and matched
        // nothing is not an error at all. It resolves, and the resolution is
        // UnknownPage -- so "did not finish" and "did not match" are told apart
        // by whether there is an error, then by which kind it carries.
        auto const completedMiss = resolve(fixture, 26, 26);
        REQUIRE(completedMiss.has_value());
        CHECK(std::holds_alternative<UnknownPage>(*completedMiss));

        // A stop reason must never spell itself the way any other one does,
        // because the rendered message is what an operator reads to know which
        // stop happened.
        CHECK(
            searchStopDescription(SadSearchStopReason::ComparisonBudgetExhausted)
            != searchStopDescription(SadSearchStopReason::TimedOut)
        );
        CHECK(
            searchStopDescription(SadSearchStopReason::Cancelled)
            != searchStopDescription(SadSearchStopReason::TimedOut)
        );
    }
}
