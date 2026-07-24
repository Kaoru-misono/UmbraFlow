#include "test-helpers.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/regression-runner.hpp>

#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
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
        constexpr auto k_templateSourceId  = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_resolvedSourceId  = "00000000-0000-0000-0000-000000000202";
        constexpr auto k_unknownSourceId   = "00000000-0000-0000-0000-000000000203";
        constexpr auto k_ambiguousSourceId = "00000000-0000-0000-0000-000000000204";
        constexpr auto k_anchorAId         = "00000000-0000-0000-0000-000000000011";
        constexpr auto k_anchorBId         = "00000000-0000-0000-0000-000000000012";
        constexpr auto k_pageAId           = "00000000-0000-0000-0000-000000000111";
        constexpr auto k_pageBId           = "00000000-0000-0000-0000-000000000112";
        constexpr auto k_resolvedCaseId    = "00000000-0000-0000-0000-000000000301";
        constexpr auto k_unknownCaseId     = "00000000-0000-0000-0000-000000000302";
        constexpr auto k_ambiguousCaseId   = "00000000-0000-0000-0000-000000000303";
        constexpr auto k_duplicateCaseId   = "00000000-0000-0000-0000-000000000304";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        struct SourceFixture final
        {
            AuthoringSource      m_source;
            AuthoringSourceAsset m_asset;
        };

        struct RegressionFixture final
        {
            AuthoringDocument                 m_document;
            std::vector<AuthoringSourceAsset> m_assets{};

            PageId m_pageA;
            PageId m_pageB;
        };

        struct RegressionFixtureOptions final
        {
            bool m_mismatchedResolvedExpectation{};
            bool m_resolvedOnly{};
            bool m_duplicateResolvedCase{};
        };

        [[nodiscard]]
        auto sourceFixture(
            std::string_view id,
            std::array<uint8, 3> gray,
            ProjectFingerprint fingerprint
        ) -> SourceFixture
        {
            auto rgba = std::vector<std::byte>{};
            rgba.reserve(gray.size() * 4U);
            for (auto const value : gray)
            {
                rgba.emplace_back(asByte(value));
                rgba.emplace_back(asByte(value));
                rgba.emplace_back(asByte(value));
                rgba.emplace_back(asByte(255));
            }
            auto encoded = image::encodeRgbaPng(
                "regression-source.png",
                3,
                1,
                rgba
            );
            REQUIRE(encoded.has_value());
            auto const hash = sha256(*encoded);
            REQUIRE(hash.has_value());
            auto const sourceId = test::sourceId(id);
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *hash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            return SourceFixture{
                .m_source = *std::move(source),
                .m_asset = AuthoringSourceAsset{
                    .m_id       = sourceId,
                    .m_pngBytes = *std::move(encoded),
                },
            };
        }

        [[nodiscard]]
        auto regressionFixture(RegressionFixtureOptions options = {})
            -> RegressionFixture
        {
            auto const fingerprint = test::fingerprint(3, 1, 96, 96);
            auto templateSource = sourceFixture(
                k_templateSourceId,
                {2, 3, 0},
                fingerprint
            );
            auto resolvedSource = sourceFixture(
                k_resolvedSourceId,
                {2, 3, 1},
                fingerprint
            );
            auto unknownSource = sourceFixture(
                k_unknownSourceId,
                {0, 0, 0},
                fingerprint
            );
            auto ambiguousSource = sourceFixture(
                k_ambiguousSourceId,
                {1, 2, 1},
                fingerprint
            );
            auto const anchorA = test::recognizerId(k_anchorAId);
            auto const anchorB = test::recognizerId(k_anchorBId);
            auto const pageA   = test::pageId(k_pageAId);
            auto const pageB   = test::pageId(k_pageBId);
            auto const resolvedExpectation = (
                options.m_mismatchedResolvedExpectation ? pageA : pageB
            );

            auto regressions = std::vector<RegressionCase>{};
            regressions.emplace_back(
                RegressionSpec{
                    .m_id             = test::regressionId(k_resolvedCaseId),
                    .m_sourceId       = resolvedSource.m_source.id(),
                    .m_classification = RegressionClassification::Positive,
                    .m_expectation    = ResolvedRegression{resolvedExpectation},
                }
            );
            if (!options.m_resolvedOnly)
            {
                regressions.emplace_back(
                    RegressionSpec{
                        .m_id             = test::regressionId(k_unknownCaseId),
                        .m_sourceId       = unknownSource.m_source.id(),
                        .m_classification = RegressionClassification::Negative,
                        .m_expectation    = UnknownRegression{},
                    }
                );
                regressions.emplace_back(
                    RegressionSpec{
                        .m_id             = test::regressionId(k_ambiguousCaseId),
                        .m_sourceId       = ambiguousSource.m_source.id(),
                        .m_classification = RegressionClassification::Confusable,
                        .m_expectation    = AmbiguousRegression{},
                    }
                );
            }
            if (options.m_duplicateResolvedCase)
            {
                regressions.emplace_back(
                    RegressionSpec{
                        .m_id             = test::regressionId(k_duplicateCaseId),
                        .m_sourceId       = resolvedSource.m_source.id(),
                        .m_classification = RegressionClassification::Positive,
                        .m_expectation    = ResolvedRegression{pageB},
                    }
                );
            }

            auto document = AuthoringDocument::create(
                test::projectId("personal.regression_runner"),
                fingerprint,
                {
                    templateSource.m_source,
                    resolvedSource.m_source,
                    unknownSource.m_source,
                    ambiguousSource.m_source,
                },
                {
                    AuthoringRecognizerSpec{
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
                        .m_sourceId = templateSource.m_source.id(),
                    },
                    AuthoringRecognizerSpec{
                        .m_definition = test::recognizer(
                            fingerprint,
                            anchorB,
                            "anchor_b",
                            AnnotationType::PageAnchor,
                            test::pixelRect(1, 0, 1, 1),
                            test::pixelRect(0, 0, 3, 1),
                            {},
                            std::nullopt,
                            test::threshold(10'000)
                        ),
                        .m_sourceId = templateSource.m_source.id(),
                    },
                },
                {
                    test::page(pageA, "page_a", {anchorA}, {anchorB}),
                    test::page(pageB, "page_b", {anchorA}),
                },
                std::move(regressions)
            );
            REQUIRE(document.has_value());
            return RegressionFixture{
                .m_document = *std::move(document),
                .m_assets = {
                    std::move(ambiguousSource.m_asset),
                    std::move(unknownSource.m_asset),
                    std::move(resolvedSource.m_asset),
                    std::move(templateSource.m_asset),
                },
                .m_pageA = pageA,
                .m_pageB = pageB,
            };
        }
    }

    TEST_CASE("authoring regressions preserve resolved unknown and ambiguous evidence")
    {
        auto const fixture = regressionFixture();
        auto const report = runAuthoringRegressions(
            fixture.m_document,
            fixture.m_assets,
            RecognitionPolicy{.m_maximumPixelComparisons = 100}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->m_completedAllCases);
        REQUIRE(report->m_cases.size() == 3U);

        auto const& resolved  = report->m_cases[0];
        auto const& unknown   = report->m_cases[1];
        auto const& ambiguous = report->m_cases[2];
        CHECK(resolved.m_classification == RegressionClassification::Positive);
        CHECK(unknown.m_classification == RegressionClassification::Negative);
        CHECK(ambiguous.m_classification == RegressionClassification::Confusable);
        CHECK(resolved.m_matchesExpectation);
        CHECK(unknown.m_matchesExpectation);
        CHECK(ambiguous.m_matchesExpectation);
        CHECK(resolved.m_attempt.m_completedPixelComparisons == 3);
        CHECK(unknown.m_attempt.m_completedPixelComparisons == 6);
        CHECK(ambiguous.m_attempt.m_completedPixelComparisons == 5);

        auto const* p_resolvedOutcome = std::get_if<PageOutcome>(
            &resolved.m_attempt.m_result
        );
        auto const* p_unknownOutcome = std::get_if<PageOutcome>(
            &unknown.m_attempt.m_result
        );
        auto const* p_ambiguousOutcome = std::get_if<PageOutcome>(
            &ambiguous.m_attempt.m_result
        );
        REQUIRE(p_resolvedOutcome != nullptr);
        REQUIRE(p_unknownOutcome != nullptr);
        REQUIRE(p_ambiguousOutcome != nullptr);
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_resolvedOutcome));
        REQUIRE(std::holds_alternative<UnknownPage>(*p_unknownOutcome));
        REQUIRE(std::holds_alternative<AmbiguousPages>(*p_ambiguousOutcome));
        CHECK(std::get<ResolvedPage>(*p_resolvedOutcome).pageId() == fixture.m_pageB);
        auto const candidates = std::get<AmbiguousPages>(
            *p_ambiguousOutcome
        ).evidence().candidatePageIds();
        REQUIRE(candidates.size() == 2U);
        CHECK(candidates[0] == fixture.m_pageA);
        CHECK(candidates[1] == fixture.m_pageB);
    }

    TEST_CASE("authoring regression mismatch does not rewrite its classification")
    {
        auto const fixture = regressionFixture(
            RegressionFixtureOptions{
                .m_mismatchedResolvedExpectation = true,
            }
        );
        auto const report = runAuthoringRegressions(
            fixture.m_document,
            fixture.m_assets,
            RecognitionPolicy{.m_maximumPixelComparisons = 100}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->m_cases.size() == 3U);
        CHECK_FALSE(report->m_cases.front().m_matchesExpectation);
        CHECK(
            report->m_cases.front().m_classification
            == RegressionClassification::Positive
        );
        CHECK(report->m_cases[1].m_matchesExpectation);
        CHECK(report->m_cases[2].m_matchesExpectation);
    }

    TEST_CASE("authoring regression interruptions mark incomplete and skip later cases")
    {
        auto const singleFixture = regressionFixture(
            RegressionFixtureOptions{.m_resolvedOnly = true}
        );
        auto const multipleFixture = regressionFixture();
        auto cancellation = std::stop_source{};
        REQUIRE(cancellation.request_stop());
        struct InterruptionCase final
        {
            RecognitionPolicy   m_policy{};
            SadSearchStopReason m_reason{};
        };
        auto const cases = std::array{
            InterruptionCase{
                .m_policy = RecognitionPolicy{
                    .m_maximumPixelComparisons = 100,
                    .m_cancellation            = cancellation.get_token(),
                },
                .m_reason = SadSearchStopReason::Cancelled,
            },
            InterruptionCase{
                .m_policy = RecognitionPolicy{
                    .m_maximumPixelComparisons = 100,
                    .m_deadline = MonotonicInstant::fromTimePoint(
                        MonotonicInstant::TimePoint{}
                    ),
                },
                .m_reason = SadSearchStopReason::TimedOut,
            },
        };

        for (auto const& entry : cases)
        {
            auto const singleReport = runAuthoringRegressions(
                singleFixture.m_document,
                singleFixture.m_assets,
                entry.m_policy
            );
            REQUIRE(singleReport.has_value());
            CHECK_FALSE(singleReport->m_completedAllCases);
            REQUIRE(singleReport->m_cases.size() == 1U);
            CHECK_FALSE(singleReport->m_cases.front().m_matchesExpectation);
            auto const* p_stop = std::get_if<PageRecognitionStop>(
                &singleReport->m_cases.front().m_attempt.m_result
            );
            REQUIRE(p_stop != nullptr);
            CHECK(p_stop->m_reason == entry.m_reason);

            auto const multipleReport = runAuthoringRegressions(
                multipleFixture.m_document,
                multipleFixture.m_assets,
                entry.m_policy
            );
            REQUIRE(multipleReport.has_value());
            CHECK_FALSE(multipleReport->m_completedAllCases);
            REQUIRE(multipleReport->m_cases.size() == 1U);
        }
    }

    TEST_CASE("authoring regression budget stops remain per-case diagnostics")
    {
        auto const fixture = regressionFixture();
        auto const report = runAuthoringRegressions(
            fixture.m_document,
            fixture.m_assets,
            RecognitionPolicy{.m_maximumPixelComparisons = 2}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->m_completedAllCases);
        REQUIRE(report->m_cases.size() == 3U);
        for (auto const& result : report->m_cases)
        {
            CHECK_FALSE(result.m_matchesExpectation);
            auto const* p_stop = std::get_if<PageRecognitionStop>(
                &result.m_attempt.m_result
            );
            REQUIRE(p_stop != nullptr);
            CHECK(p_stop->m_reason == SadSearchStopReason::ComparisonBudgetExhausted);
            CHECK(result.m_attempt.m_completedPixelComparisons == 2);
        }
    }

    TEST_CASE("authoring regressions may independently reuse one source")
    {
        auto const fixture = regressionFixture(
            RegressionFixtureOptions{.m_duplicateResolvedCase = true}
        );
        auto const report = runAuthoringRegressions(
            fixture.m_document,
            fixture.m_assets,
            RecognitionPolicy{.m_maximumPixelComparisons = 100}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->m_completedAllCases);
        REQUIRE(report->m_cases.size() == 4U);
        auto const& first = report->m_cases.front();
        auto const& last  = report->m_cases.back();
        CHECK(first.m_sourceId == last.m_sourceId);
        CHECK(first.m_id != last.m_id);
        CHECK(first.m_matchesExpectation);
        CHECK(last.m_matchesExpectation);
        auto const* p_first = std::get_if<PageOutcome>(&first.m_attempt.m_result);
        auto const* p_last  = std::get_if<PageOutcome>(&last.m_attempt.m_result);
        REQUIRE(p_first != nullptr);
        REQUIRE(p_last != nullptr);
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_first));
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_last));
        CHECK(std::get<ResolvedPage>(*p_first).pageId() == fixture.m_pageB);
        CHECK(std::get<ResolvedPage>(*p_last).pageId() == fixture.m_pageB);
    }
}
