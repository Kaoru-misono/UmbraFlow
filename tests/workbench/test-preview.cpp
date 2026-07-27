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
            annotation::AuthoringDocument    document;
            annotation::AuthoringSourceAsset asset;

            annotation::SourceId     sourceId{annotation::test::sourceId(k_sourceId)};
            annotation::RecognizerId anchorId{annotation::test::recognizerId(k_anchorId)};
            annotation::RecognizerId actionId{annotation::test::recognizerId(k_actionId)};
            annotation::PageId       pageId{annotation::test::pageId(k_pageId)};
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
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "anchor",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 3, 1)
                    ),
                    annotation::test::interactiveElement(
                        fingerprint,
                        actionId,
                        "action",
                        sourceId,
                        annotation::test::pixelRect(1, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 3, 1)
                    ),
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {
                    annotation::test::placement(
                        pageId,
                        actionId,
                        annotation::test::pixelRect(0, 0, 3, 1)
                    ),
                },
                {}
            );
            REQUIRE(document.has_value());

            return PreviewFixture{
                .document = *std::move(document),
                .asset    = annotation::AuthoringSourceAsset{
                    .id       = sourceId,
                    .pngBytes = *std::move(pngBytes),
                },
            };
        }

        [[nodiscard]]
        auto continuingPolicy(uint64 budget) -> annotation::RecognitionPolicy
        {
            return annotation::RecognitionPolicy{
                .maximumPixelComparisons = budget,
            };
        }
    }

    TEST_CASE("runPreview resolves the fixture page and reports anchor evidence")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.asset, std::size_t{1}};

        auto const preview = runPreview(
            fixture.document,
            assets,
            fixture.sourceId,
            std::nullopt,
            continuingPolicy(1000)
        );
        REQUIRE(preview.has_value());

        REQUIRE(preview->pageKind.has_value());
        CHECK(*preview->pageKind == PreviewPageKind::Resolved);
        REQUIRE(preview->resolvedPageId.has_value());
        CHECK(*preview->resolvedPageId == fixture.pageId);
        CHECK_FALSE(preview->pageStop.has_value());

        // The result is tagged with the screen it was evaluated against, so the
        // canvas overlay can tell whether its matched rectangles belong to the
        // screen on display.
        REQUIRE(preview->sourceId.has_value());
        CHECK(*preview->sourceId == fixture.sourceId);

        REQUIRE(preview->anchorRows.size() == 1U);
        auto const& row = preview->anchorRows.front();
        CHECK(row.recognizerId == fixture.anchorId);
        CHECK(row.hit);
        REQUIRE(row.sadScore.has_value());
        CHECK(row.sadScore.value() == 0);
        CHECK(row.sadScore.value() <= row.maximumSad);
        REQUIRE(row.matchedRect.has_value());
        CHECK(row.matchedRect.value() == annotation::test::pixelRect(0, 0, 1, 1));

        CHECK_FALSE(preview->actionEvidence.has_value());
    }

    TEST_CASE("runPreview evaluates a selected action target into match evidence")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.asset, std::size_t{1}};

        auto const preview = runPreview(
            fixture.document,
            assets,
            fixture.sourceId,
            fixture.actionId,
            continuingPolicy(1000)
        );
        REQUIRE(preview.has_value());
        REQUIRE(preview->actionEvidence.has_value());
        auto const& action = preview->actionEvidence.value();
        CHECK(action.recognizerId == fixture.actionId);
        CHECK(action.hit);
        REQUIRE(action.matchedRect.has_value());
        CHECK(action.matchedRect.value() == annotation::test::pixelRect(1, 0, 1, 1));
        CHECK_FALSE(preview->actionStop.has_value());
    }

    TEST_CASE("runPreview surfaces the stop reason when the budget is exhausted")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.asset, std::size_t{1}};

        auto const preview = runPreview(
            fixture.document,
            assets,
            fixture.sourceId,
            std::nullopt,
            continuingPolicy(0)
        );
        REQUIRE(preview.has_value());
        CHECK_FALSE(preview->pageKind.has_value());
        REQUIRE(preview->pageStop.has_value());
        CHECK(preview->pageStop->recognizerId == fixture.anchorId);
        CHECK(
            preview->pageStop->reason
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
            annotation::AuthoringDocument                 document;
            std::vector<annotation::AuthoringSourceAsset> assets{};
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
                    .id          = id,
                    .contentHash = *hash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
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
                        .id       = annotation::test::regressionId(k_regressionId),
                        .sourceId = sourceId,
                        .classification =
                            annotation::RegressionClassification::Positive,
                        .expectation = annotation::ResolvedRegression{
                            .pageId = pageId,
                        },
                    }
                );
                regressions.emplace_back(
                    annotation::RegressionSpec{
                        .id = annotation::test::regressionId(
                            k_otherRegressionId
                        ),
                        .sourceId = otherId,
                        .classification =
                            annotation::RegressionClassification::Positive,
                        .expectation = annotation::ResolvedRegression{
                            .pageId = otherPageId,
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
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorId,
                        "dark_mark",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 1, 1)
                    ),
                    annotation::test::anchorElement(
                        fingerprint,
                        otherAnchor,
                        "light_mark",
                        otherId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 1, 1)
                    ),
                },
                {
                    annotation::test::page(pageId, "dark", {anchorId}),
                    annotation::test::page(otherPageId, "light", {otherAnchor}),
                },
                {},
                std::move(regressions)
            );
            REQUIRE(document.has_value());

            return ModelFixture{
                .document = *std::move(document),
                .assets   = {
                    annotation::AuthoringSourceAsset{
                        .id       = sourceId,
                        .pngBytes = std::move(darkPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = otherId,
                        .pngBytes = std::move(lightPng),
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
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        REQUIRE(check->margins.size() == 2U);
        for (auto const& margin : check->margins)
        {
            REQUIRE(margin.ownSadScore.has_value());
            CHECK(*margin.ownSadScore == 0U);
            CHECK(*margin.ownSadScore <= margin.maximumSad);

            REQUIRE(margin.nearestOtherSadScore.has_value());
            CHECK(*margin.nearestOtherSadScore > margin.maximumSad);
            CHECK(margin.nearestOtherSourceId != margin.ownSourceId);
        }
    }

    TEST_CASE("runModelCheck reports each screen against the page recorded for it")
    {
        auto const fixture = modelFixture(true);

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        REQUIRE(check->screens.size() == 2U);
        for (auto const& screen : check->screens)
        {
            CHECK(screen.outcome == ScreenCheckOutcome::Correct);
            CHECK(screen.resolvedPageId == screen.expectedPageId);
        }
    }

    TEST_CASE("runModelCheck cannot judge a screen no page is recorded for")
    {
        // The resolution is still computed; there is simply nothing to compare
        // it against, which is a different thing from resolving wrongly.
        auto const fixture = modelFixture(false);

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        REQUIRE(check->screens.size() == 2U);
        for (auto const& screen : check->screens)
        {
            CHECK(screen.outcome == ScreenCheckOutcome::Unclaimed);
            CHECK_FALSE(screen.expectedPageId.has_value());
            CHECK(screen.resolvedPageId.has_value());
        }
    }

    TEST_CASE("runModelCheck reports a screen that resolves to the wrong page")
    {
        // Both screens are recorded as the same page, so one of them must be
        // reported as resolving elsewhere rather than passing quietly.
        auto fixture = modelFixture(true);
        auto draft   = makeAuthoringDraft(fixture.document);
        for (auto& regression : draft.regressions)
        {
            regression.expectation = annotation::ResolvedRegression{
                .pageId = annotation::test::pageId(k_pageId),
            };
        }
        auto rebuilt = buildAuthoringDocument(draft);
        REQUIRE(rebuilt.has_value());

        auto const check = runModelCheck(
            *rebuilt,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        auto const wrong = std::ranges::count_if(
            check->screens,
            [](ScreenCheck const& screen)
            {
                return screen.outcome == ScreenCheckOutcome::WrongPage;
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
        auto draft         = makeAuthoringDraft(fixture.document);
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id             = sharedId,
                .name           = "shared_region",
                .annotationType = annotation::AnnotationType::ActionTarget,
                .sourceId       = darkSource,
                .templateRect   = annotation::test::pixelRect(0, 0, 1, 1),
                .searchRoi      = annotation::test::pixelRect(0, 0, 1, 1),
                .similarityBasisPoints = 9'000U,
                .defaultClick   = {},
            }
        );
        // The element's template was cut from the dark screen but it is placed on
        // the light page, which is the false-green case under test.
        draft.placements.emplace_back(
            EditablePlacement{
                .pageId    = lightPage,
                .elementId = sharedId,
                .searchRoi = annotation::test::pixelRect(0, 0, 1, 1),
            }
        );
        auto const document = buildAuthoringDocument(draft);
        REQUIRE(document.has_value());

        auto const check = runModelCheck(
            *document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        auto const margin = std::ranges::find(
            check->margins,
            sharedId,
            &RecognizerMargin::recognizerId
        );
        REQUIRE(margin != check->margins.end());

        // Measured on the light screen, which is the page it is authorized on,
        // and where these dark pixels do not match.
        CHECK(margin->ownSourceId == lightSource);
        REQUIRE(margin->ownSadScore.has_value());
        CHECK(*margin->ownSadScore > margin->maximumSad);

        // The screen its template came from is now the "elsewhere" it matches
        // perfectly, which is exactly the wrong way round for a working region.
        CHECK(margin->nearestOtherSourceId == darkSource);
        REQUIRE(margin->nearestOtherSadScore.has_value());
        CHECK(*margin->nearestOtherSadScore == 0U);
    }

    TEST_CASE("runModelCheck folds a live frame into the same margins")
    {
        // The live frame is the only screen that ever changes, so it is measured
        // alongside the stills rather than reported separately. Here it is a copy
        // of the dark screen, so the dark mark must score zero on it and the
        // light mark must miss it by the same margin it misses the dark still.
        auto const fixture = modelFixture(true);
        auto const live    = fixture.assets.at(0).pngBytes;

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            live,
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        REQUIRE(check->live.has_value());
        CHECK_FALSE(check->live->stop.has_value());
        REQUIRE(check->live->resolvedPageId.has_value());
        CHECK(*check->live->resolvedPageId == annotation::test::pageId(k_pageId));

        auto const darkId  = annotation::test::recognizerId(k_anchorId);
        auto const lightId = annotation::test::recognizerId(k_otherAnchorId);
        for (auto const& margin : check->margins)
        {
            REQUIRE(margin.liveSadScore.has_value());
            if (margin.recognizerId == darkId)
            {
                CHECK(*margin.liveSadScore == 0U);
            }
            else if (margin.recognizerId == lightId)
            {
                CHECK(*margin.liveSadScore > margin.maximumSad);
            }
        }

        // The captured screens are judged exactly as they are without one.
        for (auto const& screen : check->screens)
        {
            CHECK(screen.outcome == ScreenCheckOutcome::Correct);
        }
    }

    TEST_CASE("runModelCheck reports no live screen when it was given no frame")
    {
        auto const fixture = modelFixture(true);

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());
        CHECK_FALSE(check->live.has_value());
        for (auto const& margin : check->margins)
        {
            CHECK_FALSE(margin.liveSadScore.has_value());
        }
    }

    TEST_CASE("runPreview rejects a source that is absent from the project")
    {
        auto const fixture = previewFixture();
        auto const assets  = std::span{&fixture.asset, std::size_t{1}};
        auto const missing = annotation::test::sourceId(
            "00000000-0000-0000-0000-0000000004ff"
        );

        auto const preview = runPreview(
            fixture.document,
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
