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
#include <string>
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
            AuthoringSource      source;
            AuthoringSourceAsset asset;
        };

        struct RegressionFixture final
        {
            AuthoringDocument                 document;
            std::vector<AuthoringSourceAsset> assets{};

            PageId pageA;
            PageId pageB;
        };

        struct RegressionFixtureOptions final
        {
            bool mismatchedResolvedExpectation{};
            bool resolvedOnly{};
            bool duplicateResolvedCase{};
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
                    .id          = sourceId,
                    .contentHash = *hash,
                    .fingerprint = fingerprint,
                    .provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            return SourceFixture{
                .source = *std::move(source),
                .asset = AuthoringSourceAsset{
                    .id       = sourceId,
                    .pngBytes = *std::move(encoded),
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
            auto const anchorA = test::elementId(k_anchorAId);
            auto const anchorB = test::elementId(k_anchorBId);
            auto const pageA   = test::pageId(k_pageAId);
            auto const pageB   = test::pageId(k_pageBId);
            auto const resolvedExpectation = (
                options.mismatchedResolvedExpectation ? pageA : pageB
            );

            auto regressions = std::vector<RegressionCase>{};
            regressions.emplace_back(
                RegressionSpec{
                    .id             = test::regressionId(k_resolvedCaseId),
                    .sourceId       = resolvedSource.source.id(),
                    .classification = RegressionClassification::Positive,
                    .expectation    = ResolvedRegression{resolvedExpectation},
                }
            );
            if (!options.resolvedOnly)
            {
                regressions.emplace_back(
                    RegressionSpec{
                        .id             = test::regressionId(k_unknownCaseId),
                        .sourceId       = unknownSource.source.id(),
                        .classification = RegressionClassification::Negative,
                        .expectation    = UnknownRegression{},
                    }
                );
                regressions.emplace_back(
                    RegressionSpec{
                        .id             = test::regressionId(k_ambiguousCaseId),
                        .sourceId       = ambiguousSource.source.id(),
                        .classification = RegressionClassification::Confusable,
                        .expectation    = AmbiguousRegression{},
                    }
                );
            }
            if (options.duplicateResolvedCase)
            {
                regressions.emplace_back(
                    RegressionSpec{
                        .id             = test::regressionId(k_duplicateCaseId),
                        .sourceId       = resolvedSource.source.id(),
                        .classification = RegressionClassification::Positive,
                        .expectation    = ResolvedRegression{pageB},
                    }
                );
            }

            auto const exactAnchor = [&](
                ElementId id,
                std::string name,
                PixelRect templateRect
            )
            {
                return test::element(
                    fingerprint,
                    id,
                    std::move(name),
                    test::capabilities(Identify{}),
                    test::pixelRect(0, 0, 3, 1),
                    std::vector<Appearance>{
                        test::appearance(
                            "only",
                            templateSource.source.id(),
                            templateRect,
                            test::threshold(10'000)
                        ),
                    }
                );
            };

            // page_a requires anchor_a and forbids anchor_b; page_b requires
            // anchor_a alone. Both signatures are derived from these rows, and
            // page_b borrows the anchor page_a owns.
            auto references = std::vector<PageReference>{};
            references.emplace_back(
                test::reference(
                    pageA,
                    anchorA,
                    test::identifiesAs(SignatureRole::Required)
                )
            );
            references.emplace_back(
                test::reference(
                    pageA,
                    anchorB,
                    test::identifiesAs(SignatureRole::Forbidden)
                )
            );
            references.emplace_back(
                test::reference(
                    pageB,
                    anchorA,
                    test::identifiesAs(SignatureRole::Required),
                    Holding::Referenced
                )
            );

            auto document = AuthoringDocument::create(
                test::projectId("personal.regression_runner"),
                fingerprint,
                {
                    templateSource.source,
                    resolvedSource.source,
                    unknownSource.source,
                    ambiguousSource.source,
                },
                {
                    exactAnchor(anchorA, "anchor_a", test::pixelRect(0, 0, 1, 1)),
                    exactAnchor(anchorB, "anchor_b", test::pixelRect(1, 0, 1, 1)),
                },
                {
                    test::page(pageA, "page_a"),
                    test::page(pageB, "page_b"),
                },
                std::move(references),
                std::move(regressions)
            );
            REQUIRE(document.has_value());
            return RegressionFixture{
                .document = *std::move(document),
                .assets = {
                    std::move(ambiguousSource.asset),
                    std::move(unknownSource.asset),
                    std::move(resolvedSource.asset),
                    std::move(templateSource.asset),
                },
                .pageA = pageA,
                .pageB = pageB,
            };
        }
    }

    TEST_CASE("authoring regressions preserve resolved unknown and ambiguous evidence")
    {
        auto const fixture = regressionFixture();
        auto const report = runAuthoringRegressions(
            fixture.document,
            fixture.assets,
            RecognitionPolicy{.maximumPixelComparisons = 100}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->completedAllCases);
        REQUIRE(report->cases.size() == 3U);

        auto const& resolved  = report->cases[0];
        auto const& unknown   = report->cases[1];
        auto const& ambiguous = report->cases[2];
        CHECK(resolved.classification == RegressionClassification::Positive);
        CHECK(unknown.classification == RegressionClassification::Negative);
        CHECK(ambiguous.classification == RegressionClassification::Confusable);
        CHECK(resolved.matchesExpectation);
        CHECK(unknown.matchesExpectation);
        CHECK(ambiguous.matchesExpectation);
        CHECK(resolved.attempt.completedPixelComparisons == 3);
        CHECK(unknown.attempt.completedPixelComparisons == 6);
        CHECK(ambiguous.attempt.completedPixelComparisons == 5);

        auto const* p_resolvedOutcome = std::get_if<PageOutcome>(
            &resolved.attempt.result
        );
        auto const* p_unknownOutcome = std::get_if<PageOutcome>(
            &unknown.attempt.result
        );
        auto const* p_ambiguousOutcome = std::get_if<PageOutcome>(
            &ambiguous.attempt.result
        );
        REQUIRE(p_resolvedOutcome != nullptr);
        REQUIRE(p_unknownOutcome != nullptr);
        REQUIRE(p_ambiguousOutcome != nullptr);
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_resolvedOutcome));
        REQUIRE(std::holds_alternative<UnknownPage>(*p_unknownOutcome));
        REQUIRE(std::holds_alternative<AmbiguousPages>(*p_ambiguousOutcome));
        CHECK(std::get<ResolvedPage>(*p_resolvedOutcome).pageId() == fixture.pageB);
        auto const candidates = std::get<AmbiguousPages>(
            *p_ambiguousOutcome
        ).evidence().candidatePageIds();
        REQUIRE(candidates.size() == 2U);
        CHECK(candidates[0] == fixture.pageA);
        CHECK(candidates[1] == fixture.pageB);
    }

    TEST_CASE("authoring regression mismatch does not rewrite its classification")
    {
        auto const fixture = regressionFixture(
            RegressionFixtureOptions{
                .mismatchedResolvedExpectation = true,
            }
        );
        auto const report = runAuthoringRegressions(
            fixture.document,
            fixture.assets,
            RecognitionPolicy{.maximumPixelComparisons = 100}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->cases.size() == 3U);
        CHECK_FALSE(report->cases.front().matchesExpectation);
        CHECK(
            report->cases.front().classification
            == RegressionClassification::Positive
        );
        CHECK(report->cases[1].matchesExpectation);
        CHECK(report->cases[2].matchesExpectation);
    }

    TEST_CASE("authoring regression interruptions mark incomplete and skip later cases")
    {
        auto const singleFixture = regressionFixture(
            RegressionFixtureOptions{.resolvedOnly = true}
        );
        auto const multipleFixture = regressionFixture();
        auto cancellation = std::stop_source{};
        REQUIRE(cancellation.request_stop());
        struct InterruptionCase final
        {
            RecognitionPolicy   policy{};
            SadSearchStopReason reason{};
        };
        auto const cases = std::array{
            InterruptionCase{
                .policy = RecognitionPolicy{
                    .maximumPixelComparisons = 100,
                    .cancellation            = cancellation.get_token(),
                },
                .reason = SadSearchStopReason::Cancelled,
            },
            InterruptionCase{
                .policy = RecognitionPolicy{
                    .maximumPixelComparisons = 100,
                    .deadline = MonotonicInstant::fromTimePoint(
                        MonotonicInstant::TimePoint{}
                    ),
                },
                .reason = SadSearchStopReason::TimedOut,
            },
        };

        for (auto const& entry : cases)
        {
            auto const singleReport = runAuthoringRegressions(
                singleFixture.document,
                singleFixture.assets,
                entry.policy
            );
            REQUIRE(singleReport.has_value());
            CHECK_FALSE(singleReport->completedAllCases);
            REQUIRE(singleReport->cases.size() == 1U);
            CHECK_FALSE(singleReport->cases.front().matchesExpectation);
            auto const* p_stop = std::get_if<PageRecognitionStop>(
                &singleReport->cases.front().attempt.result
            );
            REQUIRE(p_stop != nullptr);
            CHECK(p_stop->reason == entry.reason);

            auto const multipleReport = runAuthoringRegressions(
                multipleFixture.document,
                multipleFixture.assets,
                entry.policy
            );
            REQUIRE(multipleReport.has_value());
            CHECK_FALSE(multipleReport->completedAllCases);
            REQUIRE(multipleReport->cases.size() == 1U);
        }
    }

    TEST_CASE("authoring regression budget stops remain per-case diagnostics")
    {
        auto const fixture = regressionFixture();
        auto const report = runAuthoringRegressions(
            fixture.document,
            fixture.assets,
            RecognitionPolicy{.maximumPixelComparisons = 2}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->completedAllCases);
        REQUIRE(report->cases.size() == 3U);
        for (auto const& result : report->cases)
        {
            CHECK_FALSE(result.matchesExpectation);
            auto const* p_stop = std::get_if<PageRecognitionStop>(
                &result.attempt.result
            );
            REQUIRE(p_stop != nullptr);
            CHECK(p_stop->reason == SadSearchStopReason::ComparisonBudgetExhausted);
            CHECK(result.attempt.completedPixelComparisons == 2);
        }
    }

    TEST_CASE("authoring regressions may independently reuse one source")
    {
        auto const fixture = regressionFixture(
            RegressionFixtureOptions{.duplicateResolvedCase = true}
        );
        auto const report = runAuthoringRegressions(
            fixture.document,
            fixture.assets,
            RecognitionPolicy{.maximumPixelComparisons = 100}
        );
        REQUIRE(report.has_value());
        REQUIRE(report->completedAllCases);
        REQUIRE(report->cases.size() == 4U);
        auto const& first = report->cases.front();
        auto const& last  = report->cases.back();
        CHECK(first.sourceId == last.sourceId);
        CHECK(first.id != last.id);
        CHECK(first.matchesExpectation);
        CHECK(last.matchesExpectation);
        auto const* p_first = std::get_if<PageOutcome>(&first.attempt.result);
        auto const* p_last  = std::get_if<PageOutcome>(&last.attempt.result);
        REQUIRE(p_first != nullptr);
        REQUIRE(p_last != nullptr);
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_first));
        REQUIRE(std::holds_alternative<ResolvedPage>(*p_last));
        CHECK(std::get<ResolvedPage>(*p_first).pageId() == fixture.pageB);
        CHECK(std::get<ResolvedPage>(*p_last).pageId() == fixture.pageB);
    }
}
