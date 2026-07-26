#include "../annotation/test-helpers.hpp"

#include <authoring-edit.hpp>
#include <preview.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto k_sourceId   = "00000000-0000-0000-0000-000000000401";
        constexpr auto k_anchorId   = "00000000-0000-0000-0000-000000000411";
        constexpr auto k_actionId   = "00000000-0000-0000-0000-000000000412";
        constexpr auto k_pageId     = "00000000-0000-0000-0000-000000000421";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        struct PreviewFixture final
        {
            annotation::AuthoringDocument    m_document;
            annotation::AuthoringSourceAsset m_asset;

            annotation::SourceId     m_sourceId{annotation::test::sourceId(k_sourceId)};
            annotation::RecognizerId m_anchorId{annotation::test::recognizerId(k_anchorId)};
            annotation::RecognizerId m_actionId{annotation::test::recognizerId(k_actionId)};
            annotation::PageId       m_pageId{annotation::test::pageId(k_pageId)};
        };

        [[nodiscard]]
        auto previewFixture() -> PreviewFixture
        {
            auto const fingerprint = annotation::test::fingerprint(3, 1, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::recognizerId(k_anchorId);
            auto const actionId    = annotation::test::recognizerId(k_actionId);
            auto const pageId      = annotation::test::pageId(k_pageId);

            // Three distinct opaque pixels so each 1x1 template matches at exactly
            // one column of the search ROI.
            auto const rgba = std::vector{
                asByte(10), asByte(20), asByte(30), asByte(255),
                asByte(120), asByte(130), asByte(140), asByte(255),
                asByte(220), asByte(230), asByte(240), asByte(255),
            };
            auto pngBytes = image::encodeRgbaPng("preview-source.png", 3, 1, rgba);
            REQUIRE(pngBytes.has_value());
            auto const sourceHash = annotation::sha256(*pngBytes);
            REQUIRE(sourceHash.has_value());

            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *sourceHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            anchorId,
                            "anchor",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 1, 1),
                            annotation::test::pixelRect(0, 0, 3, 1)
                        ),
                        .m_sourceId = sourceId,
                    },
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            actionId,
                            "action",
                            annotation::AnnotationType::ActionTarget,
                            annotation::test::pixelRect(1, 0, 1, 1),
                            annotation::test::pixelRect(0, 0, 3, 1),
                            {pageId}
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {}
            );
            REQUIRE(document.has_value());

            return PreviewFixture{
                .m_document = *std::move(document),
                .m_asset    = annotation::AuthoringSourceAsset{
                    .m_id       = sourceId,
                    .m_pngBytes = *std::move(pngBytes),
                },
            };
        }

        [[nodiscard]]
        auto continuingPolicy(uint64 budget) -> annotation::RecognitionPolicy
        {
            return annotation::RecognitionPolicy{
                .m_maximumPixelComparisons = budget,
            };
        }
    }

    TEST_CASE("runPreview resolves the fixture page and reports anchor evidence")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.m_asset, std::size_t{1}};

        auto const preview = runPreview(
            fixture.m_document,
            assets,
            fixture.m_sourceId,
            std::nullopt,
            continuingPolicy(1000)
        );
        REQUIRE(preview.has_value());

        REQUIRE(preview->m_pageKind.has_value());
        CHECK(*preview->m_pageKind == PreviewPageKind::Resolved);
        REQUIRE(preview->m_resolvedPageId.has_value());
        CHECK(*preview->m_resolvedPageId == fixture.m_pageId);
        CHECK_FALSE(preview->m_pageStop.has_value());

        REQUIRE(preview->m_anchorRows.size() == 1U);
        auto const& row = preview->m_anchorRows.front();
        CHECK(row.m_recognizerId == fixture.m_anchorId);
        CHECK(row.m_hit);
        REQUIRE(row.m_sadScore.has_value());
        CHECK(row.m_sadScore.value() == 0);
        CHECK(row.m_sadScore.value() <= row.m_maximumSad);
        REQUIRE(row.m_matchedRect.has_value());
        CHECK(row.m_matchedRect.value() == annotation::test::pixelRect(0, 0, 1, 1));

        CHECK_FALSE(preview->m_actionEvidence.has_value());
    }

    TEST_CASE("runPreview evaluates a selected action target into match evidence")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.m_asset, std::size_t{1}};

        auto const preview = runPreview(
            fixture.m_document,
            assets,
            fixture.m_sourceId,
            fixture.m_actionId,
            continuingPolicy(1000)
        );
        REQUIRE(preview.has_value());
        REQUIRE(preview->m_actionEvidence.has_value());
        auto const& action = preview->m_actionEvidence.value();
        CHECK(action.m_recognizerId == fixture.m_actionId);
        CHECK(action.m_hit);
        REQUIRE(action.m_matchedRect.has_value());
        CHECK(action.m_matchedRect.value() == annotation::test::pixelRect(1, 0, 1, 1));
        CHECK_FALSE(preview->m_actionStop.has_value());
    }

    TEST_CASE("runPreview surfaces the stop reason when the budget is exhausted")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.m_asset, std::size_t{1}};

        auto const preview = runPreview(
            fixture.m_document,
            assets,
            fixture.m_sourceId,
            std::nullopt,
            continuingPolicy(0)
        );
        REQUIRE(preview.has_value());
        CHECK_FALSE(preview->m_pageKind.has_value());
        REQUIRE(preview->m_pageStop.has_value());
        CHECK(preview->m_pageStop->m_recognizerId == fixture.m_anchorId);
        CHECK(
            preview->m_pageStop->m_reason
            == SadSearchStopReason::ComparisonBudgetExhausted
        );
    }

    namespace
    {
        constexpr auto k_otherSourceId = "00000000-0000-0000-0000-000000000402";
        constexpr auto k_otherAnchorId = "00000000-0000-0000-0000-000000000413";
        constexpr auto k_otherPageId   = "00000000-0000-0000-0000-000000000422";
        constexpr auto k_regressionId  = "00000000-0000-0000-0000-000000000431";
        constexpr auto k_otherRegressionId =
            "00000000-0000-0000-0000-000000000432";

        struct ModelFixture final
        {
            annotation::AuthoringDocument                 m_document;
            std::vector<annotation::AuthoringSourceAsset> m_assets{};
        };

        [[nodiscard]]
        auto encodedRow(
            std::vector<std::byte> const& rgba
        ) -> std::vector<std::byte>
        {
            auto encoded = image::encodeRgbaPng("model-source.png", 3, 1, rgba);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto sourceFrom(
            annotation::SourceId id,
            annotation::ProjectFingerprint fingerprint,
            std::vector<std::byte> const& pngBytes
        ) -> annotation::AuthoringSource
        {
            auto const hash = annotation::sha256(pngBytes);
            REQUIRE(hash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .m_id          = id,
                    .m_contentHash = *hash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            return *std::move(source);
        }

        // Two screens whose first pixel differs, and one anchor per screen
        // searching only that pixel. Each anchor therefore matches its own screen
        // exactly and misses the other by a wide margin, which is the shape every
        // cross-screen check is looking for.
        [[nodiscard]]
        auto modelFixture(bool withExpectations) -> ModelFixture
        {
            auto const fingerprint  = annotation::test::fingerprint(3, 1, 96, 96);
            auto const sourceId     = annotation::test::sourceId(k_sourceId);
            auto const otherId      = annotation::test::sourceId(k_otherSourceId);
            auto const anchorId     = annotation::test::recognizerId(k_anchorId);
            auto const otherAnchor  = annotation::test::recognizerId(k_otherAnchorId);
            auto const pageId       = annotation::test::pageId(k_pageId);
            auto const otherPageId  = annotation::test::pageId(k_otherPageId);

            auto const dark = std::vector{
                asByte(10), asByte(20), asByte(30), asByte(255),
                asByte(120), asByte(130), asByte(140), asByte(255),
                asByte(220), asByte(230), asByte(240), asByte(255),
            };
            auto const light = std::vector{
                asByte(220), asByte(230), asByte(240), asByte(255),
                asByte(120), asByte(130), asByte(140), asByte(255),
                asByte(10), asByte(20), asByte(30), asByte(255),
            };
            auto darkPng  = encodedRow(dark);
            auto lightPng = encodedRow(light);

            auto regressions = std::vector<annotation::RegressionCase>{};
            if (withExpectations)
            {
                regressions.emplace_back(
                    annotation::RegressionSpec{
                        .m_id       = annotation::test::regressionId(k_regressionId),
                        .m_sourceId = sourceId,
                        .m_classification =
                            annotation::RegressionClassification::Positive,
                        .m_expectation = annotation::ResolvedRegression{
                            .m_pageId = pageId,
                        },
                    }
                );
                regressions.emplace_back(
                    annotation::RegressionSpec{
                        .m_id = annotation::test::regressionId(
                            k_otherRegressionId
                        ),
                        .m_sourceId = otherId,
                        .m_classification =
                            annotation::RegressionClassification::Positive,
                        .m_expectation = annotation::ResolvedRegression{
                            .m_pageId = otherPageId,
                        },
                    }
                );
            }

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {
                    sourceFrom(sourceId, fingerprint, darkPng),
                    sourceFrom(otherId, fingerprint, lightPng),
                },
                {
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            anchorId,
                            "dark_mark",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 1, 1),
                            annotation::test::pixelRect(0, 0, 1, 1)
                        ),
                        .m_sourceId = sourceId,
                    },
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            otherAnchor,
                            "light_mark",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 1, 1),
                            annotation::test::pixelRect(0, 0, 1, 1)
                        ),
                        .m_sourceId = otherId,
                    },
                },
                {
                    annotation::test::page(pageId, "dark", {anchorId}),
                    annotation::test::page(otherPageId, "light", {otherAnchor}),
                },
                std::move(regressions)
            );
            REQUIRE(document.has_value());

            return ModelFixture{
                .m_document = *std::move(document),
                .m_assets   = {
                    annotation::AuthoringSourceAsset{
                        .m_id       = sourceId,
                        .m_pngBytes = std::move(darkPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .m_id       = otherId,
                        .m_pngBytes = std::move(lightPng),
                    },
                },
            };
        }
    }

    TEST_CASE("runModelCheck scores every mark on the screen it did not come from")
    {
        // The point of the whole check: a mark scores zero against the image it
        // was cut from, so only its score elsewhere carries information.
        auto const fixture = modelFixture(true);

        auto const check = runModelCheck(
            fixture.m_document,
            fixture.m_assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        REQUIRE(check->m_margins.size() == 2U);
        for (auto const& margin : check->m_margins)
        {
            REQUIRE(margin.m_ownSadScore.has_value());
            CHECK(*margin.m_ownSadScore == 0U);
            CHECK(*margin.m_ownSadScore <= margin.m_maximumSad);

            REQUIRE(margin.m_nearestOtherSadScore.has_value());
            CHECK(*margin.m_nearestOtherSadScore > margin.m_maximumSad);
            CHECK(margin.m_nearestOtherSourceId != margin.m_ownSourceId);
        }
    }

    TEST_CASE("runModelCheck reports each screen against the page recorded for it")
    {
        auto const fixture = modelFixture(true);

        auto const check = runModelCheck(
            fixture.m_document,
            fixture.m_assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        REQUIRE(check->m_screens.size() == 2U);
        for (auto const& screen : check->m_screens)
        {
            CHECK(screen.m_outcome == ScreenCheckOutcome::Correct);
            CHECK(screen.m_resolvedPageId == screen.m_expectedPageId);
        }
    }

    TEST_CASE("runModelCheck cannot judge a screen no page is recorded for")
    {
        // The resolution is still computed; there is simply nothing to compare
        // it against, which is a different thing from resolving wrongly.
        auto const fixture = modelFixture(false);

        auto const check = runModelCheck(
            fixture.m_document,
            fixture.m_assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        REQUIRE(check->m_screens.size() == 2U);
        for (auto const& screen : check->m_screens)
        {
            CHECK(screen.m_outcome == ScreenCheckOutcome::Unclaimed);
            CHECK_FALSE(screen.m_expectedPageId.has_value());
            CHECK(screen.m_resolvedPageId.has_value());
        }
    }

    TEST_CASE("runModelCheck reports a screen that resolves to the wrong page")
    {
        // Both screens are recorded as the same page, so one of them must be
        // reported as resolving elsewhere rather than passing quietly.
        auto fixture = modelFixture(true);
        auto draft   = makeAuthoringDraft(fixture.m_document);
        for (auto& regression : draft.m_regressions)
        {
            regression.m_expectation = annotation::ResolvedRegression{
                .m_pageId = annotation::test::pageId(k_pageId),
            };
        }
        auto rebuilt = buildAuthoringDocument(draft);
        REQUIRE(rebuilt.has_value());

        auto const check = runModelCheck(
            *rebuilt,
            fixture.m_assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        auto const wrong = std::ranges::count_if(
            check->m_screens,
            [](ScreenCheck const& screen)
            {
                return screen.m_outcome == ScreenCheckOutcome::WrongPage;
            }
        );
        CHECK(wrong == 1);
    }

    TEST_CASE("a shared region is scored on the page it is used on")
    {
        // The false green this exists to prevent: a region whose template was cut
        // from one screen but which is authorized on another. Scoring it against
        // the screen it came from reports a perfect match for something that
        // never fires anywhere it is allowed to.
        auto const sharedId = annotation::test::recognizerId(
            "00000000-0000-0000-0000-000000000414"
        );
        auto const darkSource  = annotation::test::sourceId(k_sourceId);
        auto const lightSource = annotation::test::sourceId(k_otherSourceId);
        auto const lightPage   = annotation::test::pageId(k_otherPageId);

        auto const fixture = modelFixture(true);
        auto draft         = makeAuthoringDraft(fixture.m_document);
        draft.m_recognizers.emplace_back(
            EditableRecognizer{
                .m_id             = sharedId,
                .m_name           = "shared_region",
                .m_annotationType = annotation::AnnotationType::ActionTarget,
                .m_sourceId       = darkSource,
                .m_templateRect   = annotation::test::pixelRect(0, 0, 1, 1),
                .m_searchRoi      = annotation::test::pixelRect(0, 0, 1, 1),
                .m_similarityBasisPoints = 9'000U,
                .m_defaultClick   = {},
                .m_allowedPageIds = {lightPage},
            }
        );
        auto const document = buildAuthoringDocument(draft);
        REQUIRE(document.has_value());

        auto const check = runModelCheck(
            *document,
            fixture.m_assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        auto const margin = std::ranges::find(
            check->m_margins,
            sharedId,
            &RecognizerMargin::m_recognizerId
        );
        REQUIRE(margin != check->m_margins.end());

        // Measured on the light screen, which is the page it is authorized on,
        // and where these dark pixels do not match.
        CHECK(margin->m_ownSourceId == lightSource);
        REQUIRE(margin->m_ownSadScore.has_value());
        CHECK(*margin->m_ownSadScore > margin->m_maximumSad);

        // The screen its template came from is now the "elsewhere" it matches
        // perfectly, which is exactly the wrong way round for a working region.
        CHECK(margin->m_nearestOtherSourceId == darkSource);
        REQUIRE(margin->m_nearestOtherSadScore.has_value());
        CHECK(*margin->m_nearestOtherSadScore == 0U);
    }

    TEST_CASE("runModelCheck folds a live frame into the same margins")
    {
        // The live frame is the only screen that ever changes, so it is measured
        // alongside the stills rather than reported separately. Here it is a copy
        // of the dark screen, so the dark mark must score zero on it and the
        // light mark must miss it by the same margin it misses the dark still.
        auto const fixture = modelFixture(true);
        auto const live    = fixture.m_assets.at(0).m_pngBytes;

        auto const check = runModelCheck(
            fixture.m_document,
            fixture.m_assets,
            live,
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        REQUIRE(check->m_live.has_value());
        CHECK_FALSE(check->m_live->m_stop.has_value());
        REQUIRE(check->m_live->m_resolvedPageId.has_value());
        CHECK(*check->m_live->m_resolvedPageId == annotation::test::pageId(k_pageId));

        auto const darkId  = annotation::test::recognizerId(k_anchorId);
        auto const lightId = annotation::test::recognizerId(k_otherAnchorId);
        for (auto const& margin : check->m_margins)
        {
            REQUIRE(margin.m_liveSadScore.has_value());
            if (margin.m_recognizerId == darkId)
            {
                CHECK(*margin.m_liveSadScore == 0U);
            }
            else if (margin.m_recognizerId == lightId)
            {
                CHECK(*margin.m_liveSadScore > margin.m_maximumSad);
            }
        }

        // The captured screens are judged exactly as they are without one.
        for (auto const& screen : check->m_screens)
        {
            CHECK(screen.m_outcome == ScreenCheckOutcome::Correct);
        }
    }

    TEST_CASE("runModelCheck reports no live screen when it was given no frame")
    {
        auto const fixture = modelFixture(true);

        auto const check = runModelCheck(
            fixture.m_document,
            fixture.m_assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        CHECK_FALSE(check->m_live.has_value());
        for (auto const& margin : check->m_margins)
        {
            CHECK_FALSE(margin.m_liveSadScore.has_value());
        }
    }

    TEST_CASE("runPreview rejects a source that is absent from the project")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.m_asset, std::size_t{1}};
        auto const missing = annotation::test::sourceId(
            "00000000-0000-0000-0000-0000000004ff"
        );

        auto const preview = runPreview(
            fixture.m_document,
            assets,
            missing,
            std::nullopt,
            continuingPolicy(1000)
        );
        REQUIRE_FALSE(preview.has_value());
        annotation::test::requireErrorKind(
            preview.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
