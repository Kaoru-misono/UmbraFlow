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
        constexpr auto g_anchorAId = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_anchorBId = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_pageAId = "00000000-0000-0000-0000-000000000101";
        constexpr auto g_pageBId = "00000000-0000-0000-0000-000000000102";

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
            RecognitionCatalog m_catalog;
            RecognizerId m_anchorA{test::recognizerId(g_anchorAId)};
            RecognizerId m_anchorB{test::recognizerId(g_anchorBId)};
            PageId m_pageA{test::pageId(g_pageAId)};
            PageId m_pageB{test::pageId(g_pageBId)};
        };

        auto resolutionFixture() -> ResolutionFixture
        {
            auto const projectFingerprint = test::fingerprint();
            auto const anchorA = test::recognizerId(g_anchorAId);
            auto const anchorB = test::recognizerId(g_anchorBId);
            auto const pageA = test::pageId(g_pageAId);
            auto const pageB = test::pageId(g_pageBId);
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
                .m_catalog = test::catalog(
                    projectFingerprint,
                    std::move(recognizers),
                    std::move(pages)
                ),
                .m_anchorA = anchorA,
                .m_anchorB = anchorB,
                .m_pageA   = pageA,
                .m_pageB   = pageB,
            };
        }

        auto resolve(
            ResolutionFixture const& fixture,
            uint64 anchorAScore,
            uint64 anchorBScore
        ) -> Result<PageOutcome>
        {
            auto const* p_anchorA = fixture.m_catalog.findRecognizer(fixture.m_anchorA);
            auto const* p_anchorB = fixture.m_catalog.findRecognizer(fixture.m_anchorB);
            REQUIRE(p_anchorA != nullptr);
            REQUIRE(p_anchorB != nullptr);
            auto const evaluations = std::array{
                anchorEvaluation(*p_anchorA, anchorAScore),
                anchorEvaluation(*p_anchorB, anchorBScore),
            };
            return PageResolver::resolve(
                fixture.m_catalog,
                FrameIdentity{SessionId{7}, TargetGeneration::fromValue(3), FrameId{11}},
                evaluations
            );
        }
    }

    TEST_CASE("anchor evidence accepts the inclusive integer SAD boundary")
    {
        auto const projectFingerprint = test::fingerprint();
        auto const anchor = test::recognizer(
            projectFingerprint,
            test::recognizerId(g_anchorAId),
            "anchor",
            AnnotationType::PageAnchor,
            test::pixelRect(0, 0, 2, 2),
            test::pixelRect(0, 0, 4, 4)
        );
        struct BoundaryCase final
        {
            uint64 m_score{};
            bool m_hit{};
        };
        auto const cases = std::array{
            BoundaryCase{101, true},
            BoundaryCase{102, true},
            BoundaryCase{103, false},
        };
        for (auto const testCase : cases)
        {
            auto const evaluation = anchorEvaluation(anchor, testCase.m_score);
            auto const* p_evidence = std::get_if<AnchorEvidence>(
                &evaluation.evaluation()
            );
            REQUIRE(p_evidence != nullptr);
            CHECK(p_evidence->maximumSad() == 102);
            CHECK(p_evidence->hit() == testCase.m_hit);
            CHECK(p_evidence->sadScore() == testCase.m_score);
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
        CHECK(page.pageId() == fixture.m_pageB);
        CHECK(page.evidence().pages().size() == 2);

        auto const ambiguous = resolve(fixture, 0, 26);
        REQUIRE(ambiguous.has_value());
        REQUIRE(std::holds_alternative<AmbiguousPages>(*ambiguous));
        auto const candidates = std::get<AmbiguousPages>(
            *ambiguous
        ).evidence().candidatePageIds();
        REQUIRE(candidates.size() == 2);
        CHECK(candidates[0] == fixture.m_pageA);
        CHECK(candidates[1] == fixture.m_pageB);
    }

    TEST_CASE("every matcher stop reason aborts complete page resolution")
    {
        auto const fixture = resolutionFixture();
        auto const* p_anchor = fixture.m_catalog.findRecognizer(fixture.m_anchorA);
        REQUIRE(p_anchor != nullptr);
        struct StopCase final
        {
            SadSearchStopReason m_reason{};
            AutomationErrorKind m_expected{};
        };
        auto const cases = std::array{
            StopCase{SadSearchStopReason::Cancelled, AutomationErrorKind::Cancelled},
            StopCase{SadSearchStopReason::TimedOut, AutomationErrorKind::Timeout},
            StopCase{
                SadSearchStopReason::ComparisonBudgetExhausted,
                AutomationErrorKind::RecognitionFailed,
            },
        };
        for (auto const testCase : cases)
        {
            auto const sadOutcome = SadSearchOutcome{testCase.m_reason};
            auto const evaluation = AnchorEvaluation::fromSadOutcome(
                *p_anchor,
                sadOutcome
            );
            REQUIRE(evaluation.has_value());
            auto const evaluations = std::array{*evaluation};
            auto const result = PageResolver::resolve(
                fixture.m_catalog,
                FrameIdentity{SessionId{7}, TargetGeneration::fromValue(3), FrameId{11}},
                evaluations
            );
            REQUIRE_FALSE(result.has_value());
            test::requireErrorKind(result.error(), testCase.m_expected);
        }
    }
}
