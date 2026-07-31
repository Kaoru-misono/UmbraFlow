#include "../annotation/test-helpers.hpp"
#include "authoring-fixture.hpp"

#include <authoring-edit.hpp>
#include <preview.hpp>

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/recognition-runtime.hpp>
#include <annotation/resource.hpp>

#include <core/numeric/checked-cast.hpp>
#include <core/time/monotonic-time.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <vision/sad.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
            annotation::ElementId anchorId{annotation::test::elementId(k_anchorId)};
            annotation::ElementId actionId{annotation::test::elementId(k_actionId)};
            annotation::PageId       pageId{annotation::test::pageId(k_pageId)};
        };

        [[nodiscard]]
        auto previewFixture() -> PreviewFixture
        {
            auto const fingerprint = annotation::test::fingerprint(3, 1, 96, 96);
            auto const sourceId    = annotation::test::sourceId(k_sourceId);
            auto const anchorId    = annotation::test::elementId(k_anchorId);
            auto const actionId    = annotation::test::elementId(k_actionId);
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
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "anchor",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 3, 1)
                    ),
                    test::clickableElement(
                        fingerprint,
                        actionId,
                        "action",
                        sourceId,
                        annotation::test::pixelRect(1, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 3, 1)
                    ),
                },
                {annotation::test::page(pageId, "home")},
                {
                    annotation::test::reference(
                        pageId,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        pageId,
                        actionId,
                        annotation::test::interacts()
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

    TEST_CASE("pagePolicyFor scales the per-search budget by the anchor count")
    {
        // The named semantic of the workbench boundary: k_recognitionComparisonBudget
        // is one search's ceiling, and a page of N anchors must be given N times
        // that total, because evaluatePage shares one budget across all of them.
        auto policy     = continuingPolicy(1000);
        policy.deadline = MonotonicInstant::now();

        CHECK(pagePolicyFor(policy, 1).maximumPixelComparisons == 1000U);
        CHECK(pagePolicyFor(policy, 3).maximumPixelComparisons == 3000U);

        // The deadline and cancellation are the wall-clock and cooperative
        // guards; only the comparison ceiling scales.
        CHECK(pagePolicyFor(policy, 3).deadline == policy.deadline);

        // Overflow saturates rather than shrinking below the per-search intent:
        // a smaller budget is the starvation the scaling exists to remove.
        auto const huge = continuingPolicy(std::numeric_limits<uint64>::max());
        CHECK(
            pagePolicyFor(huge, 2).maximumPixelComparisons
            == std::numeric_limits<uint64>::max()
        );
    }

    namespace
    {
        constexpr auto k_lowAnchorId  = "00000000-0000-0000-0000-000000000441";
        constexpr auto k_highAnchorId = "00000000-0000-0000-0000-000000000442";
        constexpr auto k_screenAId    = "00000000-0000-0000-0000-000000000451";
        constexpr auto k_screenBId    = "00000000-0000-0000-0000-000000000452";
        constexpr auto k_starvePageId = "00000000-0000-0000-0000-000000000461";

        // Eight distinct 1x1 gray pixels in a row, offset per screen so that
        // neither anchor's template value (the first and last of screen A) ever
        // appears within the ROI it is searched in. That forces each 1x1 search
        // to scan its whole seven-column ROI -- one comparison per candidate,
        // seven per anchor -- and finish as a miss, which is a completed evidence
        // row. A perfect match would exit early and cost far fewer comparisons,
        // hiding the very starvation under test.
        [[nodiscard]]
        auto grayRow(uint8 base) -> std::vector<std::byte>
        {
            auto pixels = std::vector<std::byte>{};
            pixels.reserve(8U * 4U);
            for (auto column = uint8{0}; column < 8U; ++column)
            {
                auto const value = static_cast<uint8>(base + column * 30U);
                pixels.emplace_back(asByte(value));
                pixels.emplace_back(asByte(value));
                pixels.emplace_back(asByte(value));
                pixels.emplace_back(asByte(255));
            }
            return pixels;
        }

        [[nodiscard]]
        auto encodedWideRow(
            std::vector<std::byte> const& rgba
        ) -> std::vector<std::byte>
        {
            auto encoded = image::encodeRgbaPng("starve-source.png", 8, 1, rgba);
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto wideSource(
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

        struct StarvationFixture final
        {
            annotation::AuthoringDocument                 document;
            std::vector<annotation::AuthoringSourceAsset> assets{};

            annotation::SourceId screenA{annotation::test::sourceId(k_screenAId)};
        };

        // One page of two anchors, each of which costs seven comparisons to
        // search. Under a shared page total equal to one per-search budget the
        // second anchor starves; the workbench must scale the total by the anchor
        // count so both complete.
        [[nodiscard]]
        auto starvationFixture() -> StarvationFixture
        {
            auto const fingerprint = annotation::test::fingerprint(8, 1, 96, 96);
            auto const screenA     = annotation::test::sourceId(k_screenAId);
            auto const screenB     = annotation::test::sourceId(k_screenBId);
            auto const lowAnchor   = annotation::test::elementId(k_lowAnchorId);
            auto const highAnchor  = annotation::test::elementId(k_highAnchorId);
            auto const pageId      = annotation::test::pageId(k_starvePageId);

            auto pngA = encodedWideRow(grayRow(10));
            auto pngB = encodedWideRow(grayRow(15));

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {
                    wideSource(screenA, fingerprint, pngA),
                    wideSource(screenB, fingerprint, pngB),
                },
                {
                    // Template is column 0 of screen A; searched in columns 1..7,
                    // where its value never recurs, so the search misses after a
                    // full seven-column scan.
                    test::markElement(
                        fingerprint,
                        lowAnchor,
                        "low_anchor",
                        screenA,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(1, 0, 7, 1)
                    ),
                    // Template is column 7 of screen A; searched in columns 0..6.
                    test::markElement(
                        fingerprint,
                        highAnchor,
                        "high_anchor",
                        screenA,
                        annotation::test::pixelRect(7, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 7, 1)
                    ),
                },
                {annotation::test::page(pageId, "home")},
                {
                    annotation::test::reference(
                        pageId,
                        lowAnchor,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        pageId,
                        highAnchor,
                        annotation::test::identifiesAs()
                    ),
                },
                {}
            );
            REQUIRE(document.has_value());

            return StarvationFixture{
                .document = *std::move(document),
                .assets   = {
                    annotation::AuthoringSourceAsset{
                        .id       = screenA,
                        .pngBytes = std::move(pngA),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = screenB,
                        .pngBytes = std::move(pngB),
                    },
                },
            };
        }
    }

    TEST_CASE("runPreview scales the page budget so no anchor starves")
    {
        // Each anchor costs seven comparisons. A per-search budget of ten leaves
        // only three after the first anchor, so before the fix the second anchor
        // hit the shared total and the preview returned one row and a page stop.
        // The workbench now scales the page total to ten per anchor, so both
        // anchors complete and both evidence rows appear.
        auto const fixture = starvationFixture();

        auto const preview = runPreview(
            fixture.document,
            fixture.assets,
            fixture.screenA,
            std::nullopt,
            continuingPolicy(10)
        );
        REQUIRE(preview.has_value());
        CHECK_FALSE(preview->pageStop.has_value());
        CHECK(preview->anchorRows.size() == 2U);
    }

    TEST_CASE("runModelCheck scales each screen's page budget so no screen starves")
    {
        // The same starvation reaches the model check, whose per-screen page
        // evaluation shares one budget across every anchor. With two anchors of
        // seven comparisons each and a per-search budget of ten, an unscaled
        // total would stop the second anchor and report the screen as Stopped.
        auto const fixture = starvationFixture();

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(10)
        );
        REQUIRE(check.has_value());
        REQUIRE(check->screens.size() == 2U);
        for (auto const& screen : check->screens)
        {
            CHECK(screen.outcome != ScreenCheckOutcome::Stopped);
        }
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
            auto const anchorId     = annotation::test::elementId(k_anchorId);
            auto const otherAnchor  = annotation::test::elementId(k_otherAnchorId);
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
                    test::markElement(
                        fingerprint,
                        anchorId,
                        "dark_mark",
                        sourceId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 1, 1)
                    ),
                    test::markElement(
                        fingerprint,
                        otherAnchor,
                        "light_mark",
                        otherId,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 1, 1)
                    ),
                },
                {
                    annotation::test::page(pageId, "dark"),
                    annotation::test::page(otherPageId, "light"),
                },
                {
                    annotation::test::reference(
                        pageId,
                        anchorId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        otherPageId,
                        otherAnchor,
                        annotation::test::identifiesAs()
                    ),
                },
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

    TEST_CASE("a borrowed region is scored on the page it is used on")
    {
        // The false green this exists to prevent: a region whose appearance was
        // cut from one screen but which is referenced by another page. Scoring it
        // against the screen it came from reports a perfect match for something
        // that never fires anywhere it is authorised.
        auto const sharedId = annotation::test::elementId(
            "00000000-0000-0000-0000-000000000414"
        );
        auto const darkSource  = annotation::test::sourceId(k_sourceId);
        auto const lightSource = annotation::test::sourceId(k_otherSourceId);
        auto const lightPage   = annotation::test::pageId(k_otherPageId);

        auto const fixture = modelFixture(true);
        auto draft         = makeAuthoringDraft(fixture.document);
        draft.recognizers.emplace_back(
            EditableRecognizer{
                .id   = sharedId,
                .name = "shared_region",
                .capabilities = EditableCapabilities{
                    .interact = EditableInteract{},
                },
                .searchRoi = annotation::test::pixelRect(0, 0, 1, 1),
                .variants  = {
                    EditableVariant{
                        .name         = "default",
                        .sourceId     = darkSource,
                        .templateRect = annotation::test::pixelRect(0, 0, 1, 1),
                        .similarityBasisPoints = 9'000U,
                    },
                },
            }
        );
        // The appearance was cut from the dark screen but only the light page
        // references it, which is the false-green case under test.
        draft.references.emplace_back(
            EditableReference{
                .pageId    = lightPage,
                .elementId = sharedId,
                .holding   = annotation::Holding::Owned,
                .exercised = EditableExercised{
                    .interact = annotation::ExercisedInteract{},
                },
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

        auto const darkId  = annotation::test::elementId(k_anchorId);
        auto const lightId = annotation::test::elementId(k_otherAnchorId);
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

    namespace
    {
        constexpr auto k_spareSourceId = "00000000-0000-0000-0000-000000000403";
        constexpr auto k_menuId        = "00000000-0000-0000-0000-000000000415";
        constexpr auto k_darkMarkId    = "00000000-0000-0000-0000-000000000416";
        constexpr auto k_lightMarkId   = "00000000-0000-0000-0000-000000000417";
        constexpr auto k_darkPageId    = "00000000-0000-0000-0000-000000000423";
        constexpr auto k_lightPageId   = "00000000-0000-0000-0000-000000000424";
        constexpr auto k_darkRegId     = "00000000-0000-0000-0000-000000000433";
        constexpr auto k_lightRegId    = "00000000-0000-0000-0000-000000000434";

        struct MultiPlacedFixture final
        {
            annotation::AuthoringDocument                 document;
            std::vector<annotation::AuthoringSourceAsset> assets{};

            annotation::SourceId     darkSource{annotation::test::sourceId(k_sourceId)};
            annotation::SourceId     lightSource{annotation::test::sourceId(k_otherSourceId)};
            annotation::SourceId     spareSource{annotation::test::sourceId(k_spareSourceId)};
            annotation::ElementId menuId{annotation::test::elementId(k_menuId)};
            annotation::PageId       darkPageId{annotation::test::pageId(k_darkPageId)};
            annotation::PageId       lightPageId{annotation::test::pageId(k_lightPageId)};
        };

        // One interactive element "menu" referenced by two pages, each with its
        // own claimed screen, plus a third screen no page claims. The menu
        // appearance is pixel B, which sits at a DIFFERENT column on each claimed
        // screen, so a per-screen search lands its box in a different place. The
        // element is one recognizer under its own id, which is the id every
        // consumer keys evidence by.
        [[nodiscard]]
        auto multiPlacedFixture() -> MultiPlacedFixture
        {
            auto const fingerprint = annotation::test::fingerprint(3, 1, 96, 96);
            auto const a = std::vector{asByte(10), asByte(20), asByte(30), asByte(255)};
            auto const b = std::vector{asByte(120), asByte(130), asByte(140), asByte(255)};
            auto const c = std::vector{asByte(220), asByte(230), asByte(240), asByte(255)};
            auto const y = std::vector{asByte(200), asByte(100), asByte(50), asByte(255)};
            auto const row = [](
                std::vector<std::byte> const& p0,
                std::vector<std::byte> const& p1,
                std::vector<std::byte> const& p2
            ) -> std::vector<std::byte>
            {
                auto out = std::vector<std::byte>{};
                out.insert(out.end(), p0.begin(), p0.end());
                out.insert(out.end(), p1.begin(), p1.end());
                out.insert(out.end(), p2.begin(), p2.end());
                return out;
            };

            // dark = [A, B, C] -> menu (B) matches at col 1
            // light = [B, C, Y] -> menu (B) matches at col 0
            // spare = [A, A, A] -> no page claims it
            auto darkPng  = encodedRow(row(a, b, c));
            auto lightPng = encodedRow(row(b, c, y));
            auto sparePng = encodedRow(row(a, a, a));

            auto const darkSource  = annotation::test::sourceId(k_sourceId);
            auto const lightSource = annotation::test::sourceId(k_otherSourceId);
            auto const spareSource = annotation::test::sourceId(k_spareSourceId);
            auto const menuId      = annotation::test::elementId(k_menuId);
            auto const darkMarkId  = annotation::test::elementId(k_darkMarkId);
            auto const lightMarkId = annotation::test::elementId(k_lightMarkId);
            auto const darkPageId  = annotation::test::pageId(k_darkPageId);
            auto const lightPageId = annotation::test::pageId(k_lightPageId);

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {
                    sourceFrom(darkSource, fingerprint, darkPng),
                    sourceFrom(lightSource, fingerprint, lightPng),
                    sourceFrom(spareSource, fingerprint, sparePng),
                },
                {
                    // dark_mark identifies the dark screen (A at col 0 only there).
                    test::markElement(
                        fingerprint,
                        darkMarkId,
                        "dark_mark",
                        darkSource,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 1, 1)
                    ),
                    // light_mark identifies the light screen (Y at col 2 only there).
                    test::markElement(
                        fingerprint,
                        lightMarkId,
                        "light_mark",
                        lightSource,
                        annotation::test::pixelRect(2, 0, 1, 1),
                        annotation::test::pixelRect(2, 0, 1, 1)
                    ),
                    // menu: appearance cut from the dark screen at col 1 (pixel B).
                    test::clickableElement(
                        fingerprint,
                        menuId,
                        "menu",
                        darkSource,
                        annotation::test::pixelRect(1, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 3, 1)
                    ),
                },
                {
                    annotation::test::page(darkPageId, "dark"),
                    annotation::test::page(lightPageId, "light"),
                },
                {
                    annotation::test::reference(
                        darkPageId,
                        darkMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        lightPageId,
                        lightMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        darkPageId,
                        menuId,
                        annotation::test::interacts()
                    ),
                    annotation::test::reference(
                        lightPageId,
                        menuId,
                        annotation::test::interacts(),
                        annotation::Holding::Referenced
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_darkRegId),
                            .sourceId = darkSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = darkPageId,
                            },
                        }
                    },
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_lightRegId),
                            .sourceId = lightSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = lightPageId,
                            },
                        }
                    },
                }
            );
            REQUIRE(document.has_value());

            return MultiPlacedFixture{
                .document = *std::move(document),
                .assets   = {
                    annotation::AuthoringSourceAsset{
                        .id       = darkSource,
                        .pngBytes = std::move(darkPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = lightSource,
                        .pngBytes = std::move(lightPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = spareSource,
                        .pngBytes = std::move(sparePng),
                    },
                },
            };
        }
    }

    TEST_CASE("runPreview evaluates an element referenced by two pages on each")
    {
        // One element is one recognizer under its own id, so the id the UI
        // selected it by is the id the runtime answers to. What the shown
        // screen's page still decides is which reference supplies the region,
        // and each page's reference lands the box where that screen has it.
        auto const fixture = multiPlacedFixture();

        auto const onDark = runPreview(
            fixture.document,
            fixture.assets,
            fixture.darkSource,
            fixture.menuId,
            continuingPolicy(1000)
        );
        REQUIRE(onDark.has_value());
        REQUIRE(onDark->actionEvidence.has_value());
        CHECK(onDark->actionEvidence->recognizerId == fixture.menuId);
        CHECK(onDark->actionEvidence->hit);
        REQUIRE(onDark->actionEvidence->matchedRect.has_value());
        CHECK(
            onDark->actionEvidence->matchedRect.value()
            == annotation::test::pixelRect(1, 0, 1, 1)
        );

        auto const onLight = runPreview(
            fixture.document,
            fixture.assets,
            fixture.lightSource,
            fixture.menuId,
            continuingPolicy(1000)
        );
        REQUIRE(onLight.has_value());
        REQUIRE(onLight->actionEvidence.has_value());
        CHECK(onLight->actionEvidence->recognizerId == fixture.menuId);
        CHECK(onLight->actionEvidence->hit);
        REQUIRE(onLight->actionEvidence->matchedRect.has_value());
        // The same element's box lands at a different column, because that is
        // where the light screen has these pixels.
        CHECK(
            onLight->actionEvidence->matchedRect.value()
            == annotation::test::pixelRect(0, 0, 1, 1)
        );
    }

    TEST_CASE("runPreview still searches an action target on an unclaimed screen")
    {
        // A screen no page claims supplies no reference, so the element's own
        // page supplies one and the search runs anyway. That is the whole
        // question an author looking at a foreign screen is asking -- do these
        // pixels turn up here -- and refusing to answer it would hide exactly
        // the misfire the model check hunts for.
        auto const fixture = multiPlacedFixture();

        auto const onSpare = runPreview(
            fixture.document,
            fixture.assets,
            fixture.spareSource,
            fixture.menuId,
            continuingPolicy(1000)
        );
        REQUIRE(onSpare.has_value());
        REQUIRE(onSpare->actionEvidence.has_value());
        CHECK(onSpare->actionEvidence->recognizerId == fixture.menuId);
        // The spare screen is three copies of pixel A and the menu is pixel B,
        // so the honest answer is a measured miss rather than a skipped search.
        CHECK_FALSE(onSpare->actionEvidence->hit);
        // The anchor rows are still there.
        CHECK_FALSE(onSpare->anchorRows.empty());
    }

    TEST_CASE("runModelCheck files an element's margin under the element id")
    {
        // The model check evaluates each action target per screen and records
        // the score under the element id the UI keys margins by, which is also
        // the only id the runtime catalog answers to.
        auto const fixture = multiPlacedFixture();

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        auto const margin = std::ranges::find(
            check->margins,
            fixture.menuId,
            &RecognizerMargin::recognizerId
        );
        REQUIRE(margin != check->margins.end());
        // The template (pixel B) matches on both claimed screens, so its own
        // score is a clean zero under the element id.
        REQUIRE(margin->ownSadScore.has_value());
        CHECK(*margin->ownSadScore == 0U);
    }

    namespace
    {
        // The element's own row: the fold across every appearance, which is what
        // the runtime answers with.
        [[nodiscard]]
        auto findCell(
            ModelCheck const& check,
            annotation::ElementId element,
            annotation::SourceId screen
        ) -> ModelCheckCell const*
        {
            auto const found = std::ranges::find_if(
                check.cells,
                [&](ModelCheckCell const& cell)
                {
                    return cell.subject == ModelCellSubject::Element
                        && cell.elementId == element
                        && cell.screenId == screen;
                }
            );
            return found == check.cells.end() ? nullptr : &*found;
        }

        // One appearance's own row, searched alone.
        [[nodiscard]]
        auto findAppearanceCell(
            ModelCheck const& check,
            annotation::ElementId element,
            annotation::SourceId screen,
            std::string_view appearance
        ) -> ModelCheckCell const*
        {
            auto const found = std::ranges::find_if(
                check.cells,
                [&](ModelCheckCell const& cell)
                {
                    return cell.subject == ModelCellSubject::Appearance
                        && cell.elementId == element
                        && cell.screenId == screen
                        && cell.appearance.has_value()
                        && cell.appearance->value() == appearance;
                }
            );
            return found == check.cells.end() ? nullptr : &*found;
        }

        [[nodiscard]]
        auto measuredCell(
            ModelCellOutcome outcome,
            uint64 score,
            uint64 maximumSad,
            bool expectedHit
        ) -> ModelCheckCell
        {
            return ModelCheckCell{
                .elementId   = annotation::test::elementId(k_anchorId),
                .screenId    = annotation::test::sourceId(k_sourceId),
                .outcome     = outcome,
                .sadScore    = std::optional<uint64>{score},
                .maximumSad  = maximumSad,
                .expectedHit = expectedHit,
            };
        }
    }

    TEST_CASE("classifyModelCell reads outcome against authored membership")
    {
        // Not searched and stopped are fixed states, independent of any score.
        CHECK(
            classifyModelCell(
                ModelCheckCell{
                    .elementId = annotation::test::elementId(k_anchorId),
                    .screenId  = annotation::test::sourceId(k_sourceId),
                    .outcome   = ModelCellOutcome::NotSearchedHere,
                }
            )
            == ModelCellColor::NotSearched
        );
        CHECK(
            classifyModelCell(
                ModelCheckCell{
                    .elementId = annotation::test::elementId(k_anchorId),
                    .screenId  = annotation::test::sourceId(k_sourceId),
                    .outcome   = ModelCellOutcome::Stopped,
                }
            )
            == ModelCellColor::Thin
        );

        // A hit where the mark is authored, clear of the threshold, is expected;
        // barely under it is thin; on a screen it is not authored on it is a
        // misfire regardless of how clean the score looks.
        CHECK(
            classifyModelCell(measuredCell(ModelCellOutcome::Hit, 0U, 200U, true))
            == ModelCellColor::Expected
        );
        CHECK(
            classifyModelCell(measuredCell(ModelCellOutcome::Hit, 195U, 200U, true))
            == ModelCellColor::Thin
        );
        CHECK(
            classifyModelCell(measuredCell(ModelCellOutcome::Hit, 0U, 200U, false))
            == ModelCellColor::Misfire
        );

        // A clean miss where the mark is not authored is the expected outcome;
        // barely over the threshold is thin; a miss where the mark IS authored is
        // a hole in its own page.
        CHECK(
            classifyModelCell(measuredCell(ModelCellOutcome::Miss, 400U, 200U, false))
            == ModelCellColor::Expected
        );
        CHECK(
            classifyModelCell(measuredCell(ModelCellOutcome::Miss, 210U, 200U, false))
            == ModelCellColor::Thin
        );
        CHECK(
            classifyModelCell(measuredCell(ModelCellOutcome::Miss, 400U, 200U, true))
            == ModelCellColor::Misfire
        );
    }

    TEST_CASE("runModelCheck fills the grid for a mark searched on every screen")
    {
        // A single-placement anchor is scored on every screen, so it has a cell
        // on each: a hit on its own page's screen and a clean miss elsewhere,
        // and no not-searched holes.
        auto const fixture = modelFixture(true);
        auto const dark    = annotation::test::sourceId(k_sourceId);
        auto const light   = annotation::test::sourceId(k_otherSourceId);
        auto const darkId  = annotation::test::elementId(k_anchorId);

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        auto const* p_own = findCell(*check, darkId, dark);
        REQUIRE(p_own != nullptr);
        CHECK(p_own->outcome == ModelCellOutcome::Hit);
        CHECK(p_own->expectedHit);
        CHECK(classifyModelCell(*p_own) == ModelCellColor::Expected);

        auto const* p_elsewhere = findCell(*check, darkId, light);
        REQUIRE(p_elsewhere != nullptr);
        CHECK(p_elsewhere->outcome == ModelCellOutcome::Miss);
        CHECK_FALSE(p_elsewhere->expectedHit);
        CHECK(classifyModelCell(*p_elsewhere) == ModelCellColor::Expected);
    }

    TEST_CASE("runModelCheck searches an element on screens it does not belong to")
    {
        // The grid's off-diagonal cells are the ones that carry information: a
        // mark always matches the screen it was cut from, so the only evidence
        // it identifies one screen rather than another is what it does on the
        // rest. The menu is referenced by the dark and light pages and by no
        // other, and the spare screen must still be MEASURED -- an unsearched
        // cell there could never report a hit where the element does not belong.
        auto const fixture = multiPlacedFixture();

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(1000)
        );
        REQUIRE(check.has_value());

        auto const* p_dark = findCell(*check, fixture.menuId, fixture.darkSource);
        REQUIRE(p_dark != nullptr);
        CHECK(p_dark->outcome == ModelCellOutcome::Hit);
        CHECK(p_dark->expectedHit);

        auto const* p_light = findCell(*check, fixture.menuId, fixture.lightSource);
        REQUIRE(p_light != nullptr);
        CHECK(p_light->outcome == ModelCellOutcome::Hit);
        CHECK(p_light->expectedHit);

        auto const* p_spare = findCell(*check, fixture.menuId, fixture.spareSource);
        REQUIRE(p_spare != nullptr);
        CHECK(p_spare->outcome == ModelCellOutcome::Miss);
        CHECK_FALSE(p_spare->expectedHit);
        CHECK(p_spare->sadScore.has_value());
        CHECK(classifyModelCell(*p_spare) == ModelCellColor::Expected);
    }

    TEST_CASE("runModelCheck records a stopped anchor as a stopped cell")
    {
        // A zero comparison budget stops the page evaluation before any anchor
        // completes, so every anchor cell is Stopped rather than a missing hole.
        auto const fixture = modelFixture(true);
        auto const dark    = annotation::test::sourceId(k_sourceId);
        auto const darkId  = annotation::test::elementId(k_anchorId);

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(0)
        );
        REQUIRE(check.has_value());

        auto const* p_cell = findCell(*check, darkId, dark);
        REQUIRE(p_cell != nullptr);
        CHECK(p_cell->outcome == ModelCellOutcome::Stopped);
        CHECK(classifyModelCell(*p_cell) == ModelCellColor::Thin);
    }

    namespace
    {
        constexpr auto k_backId     = "00000000-0000-0000-0000-000000000418";
        constexpr auto k_noneSource = "00000000-0000-0000-0000-000000000404";

        // A screen as one row of opaque grey pixels. Grey because the matcher's
        // luma weights sum to 256, so a pixel written R=G=B=v converts to grey
        // exactly v and every SAD below can be worked out by hand from the
        // numbers in the fixture.
        [[nodiscard]]
        auto greyRow(std::vector<uint8> const& values) -> std::vector<std::byte>
        {
            auto rgba = std::vector<std::byte>{};
            rgba.reserve(values.size() * 4U);
            for (auto const value : values)
            {
                rgba.emplace_back(asByte(value));
                rgba.emplace_back(asByte(value));
                rgba.emplace_back(asByte(value));
                rgba.emplace_back(asByte(255));
            }
            return rgba;
        }

        [[nodiscard]]
        auto encodedGreyRow(
            std::vector<uint8> const& values
        ) -> std::vector<std::byte>
        {
            auto const width = checkedCast<uint32>(values.size());
            REQUIRE(width.has_value());
            auto encoded = image::encodeRgbaPng(
                "appearance-source.png",
                *width,
                1,
                greyRow(values)
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto colourKey(uint8 grey, uint32 tolerance) -> annotation::ColourKey
        {
            auto const key = annotation::ColourKey::create(
                grey,
                grey,
                grey,
                tolerance
            );
            REQUIRE(key.has_value());
            return *key;
        }

        struct AppearanceFixture final
        {
            annotation::AuthoringDocument                 document;
            std::vector<annotation::AuthoringSourceAsset> assets{};

            annotation::SourceId darkSource{annotation::test::sourceId(k_sourceId)};
            annotation::SourceId lightSource{
                annotation::test::sourceId(k_otherSourceId)
            };
            annotation::SourceId noneSource{annotation::test::sourceId(k_noneSource)};

            annotation::ElementId backId{annotation::test::elementId(k_backId)};
        };

        // The plan's own example, in eight pixels: one element "back" wearing a
        // different appearance on each of two screens, plus a third screen that
        // has no such button and belongs to nobody.
        //
        // The appearance list is a parameter because every red proof below is
        // the same three screens with a differently authored set of appearances
        // -- an over-broad one, a nearly-empty mask -- and rebuilding the pages,
        // marks and regressions around each would hide what actually differs.
        //
        // dark  = 10 20 30 40 50 60 70 80
        // light = 200 210 220 10 240 250 190 180
        // none  = 100 x8
        //
        // The lone 10 on the light screen is load-bearing: it is the patch of
        // "saturated" colour a tiny mask can find anywhere, and without it the
        // pathological appearance would fail for want of anything to latch onto
        // rather than for the reason the pitfall records.
        [[nodiscard]]
        auto appearanceFixture(
            std::vector<annotation::Variant> backAppearances
        ) -> AppearanceFixture
        {
            auto const fingerprint = annotation::test::fingerprint(8, 1, 96, 96);

            auto darkPng  = encodedGreyRow({10, 20, 30, 40, 50, 60, 70, 80});
            auto lightPng = encodedGreyRow({200, 210, 220, 10, 240, 250, 190, 180});
            auto nonePng  = encodedGreyRow({100, 100, 100, 100, 100, 100, 100, 100});

            auto const darkSource  = annotation::test::sourceId(k_sourceId);
            auto const lightSource = annotation::test::sourceId(k_otherSourceId);
            auto const noneSource  = annotation::test::sourceId(k_noneSource);
            auto const backId      = annotation::test::elementId(k_backId);
            auto const darkMarkId  = annotation::test::elementId(k_darkMarkId);
            auto const lightMarkId = annotation::test::elementId(k_lightMarkId);
            auto const darkPageId  = annotation::test::pageId(k_darkPageId);
            auto const lightPageId = annotation::test::pageId(k_lightPageId);

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {
                    sourceFrom(darkSource, fingerprint, darkPng),
                    sourceFrom(lightSource, fingerprint, lightPng),
                    sourceFrom(noneSource, fingerprint, nonePng),
                },
                {
                    // Column 4 reads 50 / 240 / 100 across the three screens, so
                    // one pinned pixel there identifies each claimed screen and
                    // misses the other two by more than seven thresholds.
                    test::markElement(
                        fingerprint,
                        darkMarkId,
                        "dark_mark",
                        darkSource,
                        annotation::test::pixelRect(4, 0, 1, 1),
                        annotation::test::pixelRect(4, 0, 1, 1)
                    ),
                    test::markElement(
                        fingerprint,
                        lightMarkId,
                        "light_mark",
                        lightSource,
                        annotation::test::pixelRect(4, 0, 1, 1),
                        annotation::test::pixelRect(4, 0, 1, 1)
                    ),
                    annotation::test::element(
                        fingerprint,
                        backId,
                        "back",
                        annotation::test::capabilities(
                            std::nullopt,
                            annotation::Interact{}
                        ),
                        annotation::test::pixelRect(0, 0, 8, 1),
                        std::move(backAppearances)
                    ),
                },
                {
                    annotation::test::page(darkPageId, "dark"),
                    annotation::test::page(lightPageId, "light"),
                },
                {
                    annotation::test::reference(
                        darkPageId,
                        darkMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        lightPageId,
                        lightMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        darkPageId,
                        backId,
                        annotation::test::interacts()
                    ),
                    annotation::test::reference(
                        lightPageId,
                        backId,
                        annotation::test::interacts(),
                        annotation::Holding::Referenced
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_darkRegId),
                            .sourceId = darkSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = darkPageId,
                            },
                        }
                    },
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_lightRegId),
                            .sourceId = lightSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = lightPageId,
                            },
                        }
                    },
                }
            );
            REQUIRE(document.has_value());

            return AppearanceFixture{
                .document = *std::move(document),
                .assets   = {
                    annotation::AuthoringSourceAsset{
                        .id       = darkSource,
                        .pngBytes = std::move(darkPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = lightSource,
                        .pngBytes = std::move(lightPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = noneSource,
                        .pngBytes = std::move(nonePng),
                    },
                },
            };
        }

        // The healthy pair: each appearance cut from the screen it serves, two
        // pixels wide, at a threshold that leaves a SAD ceiling of 51.
        [[nodiscard]]
        auto healthyAppearances() -> std::vector<annotation::Variant>
        {
            auto appearances = std::vector<annotation::Variant>{};
            appearances.emplace_back(
                annotation::test::variant(
                    "on_dark",
                    annotation::test::sourceId(k_sourceId),
                    annotation::test::pixelRect(0, 0, 2, 1)
                )
            );
            appearances.emplace_back(
                annotation::test::variant(
                    "on_light",
                    annotation::test::sourceId(k_otherSourceId),
                    annotation::test::pixelRect(0, 0, 2, 1)
                )
            );
            return appearances;
        }

        [[nodiscard]]
        auto healthyCheck(AppearanceFixture const& fixture) -> ModelCheck
        {
            auto check = runModelCheck(
                fixture.document,
                fixture.assets,
                {},
                continuingPolicy(10'000)
            );
            REQUIRE(check.has_value());
            return *std::move(check);
        }
    }

    TEST_CASE("every appearance is measured on every screen, diagonal and not")
    {
        // P1. The natural authoring habit is to try the dark appearance on the
        // dark screen and stop, which never asks the question that matters. Four
        // measurements exist here, and the two off-diagonal ones are the only
        // evidence that either appearance discriminates at all.
        auto const fixture = appearanceFixture(healthyAppearances());
        auto const check   = healthyCheck(fixture);

        struct Expectation final
        {
            annotation::SourceId screen;
            std::string_view     appearance;
            bool                 owns{};
        };
        auto const expectations = std::array{
            Expectation{
                .screen     = fixture.darkSource,
                .appearance = "on_dark",
                .owns       = true,
            },
            Expectation{
                .screen     = fixture.darkSource,
                .appearance = "on_light",
                .owns       = false,
            },
            Expectation{
                .screen     = fixture.lightSource,
                .appearance = "on_dark",
                .owns       = false,
            },
            Expectation{
                .screen     = fixture.lightSource,
                .appearance = "on_light",
                .owns       = true,
            },
            Expectation{
                .screen     = fixture.noneSource,
                .appearance = "on_dark",
                .owns       = false,
            },
            Expectation{
                .screen     = fixture.noneSource,
                .appearance = "on_light",
                .owns       = false,
            },
        };

        for (auto const& expectation : expectations)
        {
            CAPTURE(expectation.appearance);
            auto const* p_cell = findAppearanceCell(
                check,
                fixture.backId,
                expectation.screen,
                expectation.appearance
            );
            REQUIRE(p_cell != nullptr);
            CHECK(p_cell->expectedHit == expectation.owns);
            CHECK(
                (p_cell->outcome == ModelCellOutcome::Hit) == expectation.owns
            );
            CHECK(p_cell->sadScore.has_value());
            CHECK(classifyModelCell(*p_cell) == ModelCellColor::Expected);
        }
    }

    TEST_CASE("the folded element misses a screen no appearance of it owns")
    {
        // P2. Implied by P1, and asserted separately anyway: the fold is its own
        // code, and a fold that answered "hit" whenever any appearance scored
        // would pass every appearance row above and still be broken here.
        auto const fixture = appearanceFixture(healthyAppearances());
        auto const check   = healthyCheck(fixture);

        auto const* p_folded = findCell(check, fixture.backId, fixture.noneSource);
        REQUIRE(p_folded != nullptr);
        CHECK(p_folded->outcome == ModelCellOutcome::Miss);
        CHECK_FALSE(p_folded->expectedHit);
        CHECK(classifyModelCell(*p_folded) == ModelCellColor::Expected);

        CHECK(judgeModelCheck(check).empty());
    }

    TEST_CASE("the fold answers with the appearance the screen belongs to")
    {
        // P3. Identity AND rectangle, because the click is derived from the
        // rectangle: an element that reports the right appearance at another
        // appearance's box has already moved the click, and nothing downstream
        // can tell.
        auto const fixture = appearanceFixture(healthyAppearances());
        auto const check   = healthyCheck(fixture);

        auto const owned = std::array{
            std::pair{fixture.darkSource, std::string_view{"on_dark"}},
            std::pair{fixture.lightSource, std::string_view{"on_light"}},
        };
        for (auto const& [screen, appearance] : owned)
        {
            CAPTURE(appearance);
            auto const* p_folded = findCell(check, fixture.backId, screen);
            REQUIRE(p_folded != nullptr);
            CHECK(p_folded->outcome == ModelCellOutcome::Hit);
            REQUIRE(p_folded->appearance.has_value());
            CHECK(p_folded->appearance->value() == appearance);

            auto const* p_own = findAppearanceCell(
                check,
                fixture.backId,
                screen,
                appearance
            );
            REQUIRE(p_own != nullptr);
            CHECK(p_folded->matchedRect == p_own->matchedRect);
        }
    }

    TEST_CASE("a nearly-empty mask misfires on a screen it does not own")
    {
        // R1. The pitfall's 27-of-920 white mask, in miniature: a four-pixel
        // template whose colour key keeps exactly one saturated pixel. It does
        // not have to survive where the glyph was -- it only has to find some
        // offset in the region where that one pixel lands on its own colour, and
        // the light screen has one at column three.
        //
        // Delete the off-diagonal from the grid and this cannot fail: the cell
        // it reads does not exist.
        auto appearances = std::vector<annotation::Variant>{};
        appearances.emplace_back(
            annotation::test::variant(
                "on_dark_tiny",
                annotation::test::sourceId(k_sourceId),
                annotation::test::pixelRect(0, 0, 4, 1),
                annotation::test::threshold(),
                colourKey(10, 0)
            )
        );
        appearances.emplace_back(
            annotation::test::variant(
                "on_light",
                annotation::test::sourceId(k_otherSourceId),
                annotation::test::pixelRect(0, 0, 2, 1)
            )
        );

        auto const fixture = appearanceFixture(std::move(appearances));
        auto const check   = healthyCheck(fixture);

        // On its own screen it looks perfect, which is the whole trap.
        auto const* p_own = findAppearanceCell(
            check,
            fixture.backId,
            fixture.darkSource,
            "on_dark_tiny"
        );
        REQUIRE(p_own != nullptr);
        CHECK(p_own->outcome == ModelCellOutcome::Hit);
        CHECK(classifyModelCell(*p_own) == ModelCellColor::Expected);

        auto const* p_foreign = findAppearanceCell(
            check,
            fixture.backId,
            fixture.lightSource,
            "on_dark_tiny"
        );
        REQUIRE(p_foreign != nullptr);
        CHECK(p_foreign->outcome == ModelCellOutcome::Hit);
        CHECK_FALSE(p_foreign->expectedHit);
        CHECK(classifyModelCell(*p_foreign) == ModelCellColor::Misfire);

        auto const findings = judgeModelCheck(check);
        auto const misfire  = std::ranges::find_if(
            findings,
            [&](ModelFinding const& finding)
            {
                return finding.kind == ModelFindingKind::WrongOutcome
                    && finding.screenId == fixture.lightSource
                    && finding.appearance.has_value()
                    && finding.appearance->value() == "on_dark_tiny";
            }
        );
        CHECK(misfire != findings.end());
    }

    TEST_CASE("an over-broad appearance declared first does not answer for a narrow one")
    {
        // R2. "wide" is declared FIRST and passes its own loose threshold on the
        // light screen; "narrow" is declared second and matches there exactly.
        // Under first-past-the-threshold the fold answers "wide", at wide's own
        // rectangle three columns away -- and resolveClickPixel would take the
        // click there. Declaration order settles ties and nothing else, so the
        // answer has to be "narrow" at column zero.
        auto appearances = std::vector<annotation::Variant>{};
        appearances.emplace_back(
            annotation::test::variant(
                "wide",
                annotation::test::sourceId(k_sourceId),
                annotation::test::pixelRect(0, 0, 4, 1),
                annotation::test::threshold(2'000)
            )
        );
        appearances.emplace_back(
            annotation::test::variant(
                "narrow",
                annotation::test::sourceId(k_otherSourceId),
                annotation::test::pixelRect(0, 0, 2, 1)
            )
        );

        auto const fixture = appearanceFixture(std::move(appearances));
        auto const check   = healthyCheck(fixture);

        // The premise: the early appearance really does pass the threshold here,
        // so "first past the threshold" has something to answer with.
        auto const* p_wide = findAppearanceCell(
            check,
            fixture.backId,
            fixture.lightSource,
            "wide"
        );
        REQUIRE(p_wide != nullptr);
        REQUIRE(p_wide->outcome == ModelCellOutcome::Hit);
        REQUIRE(p_wide->matchedRect.has_value());
        CHECK(*p_wide->matchedRect == annotation::test::pixelRect(3, 0, 4, 1));

        auto const* p_folded = findCell(check, fixture.backId, fixture.lightSource);
        REQUIRE(p_folded != nullptr);
        REQUIRE(p_folded->appearance.has_value());
        CHECK(p_folded->appearance->value() == "narrow");
        REQUIRE(p_folded->matchedRect.has_value());
        CHECK(*p_folded->matchedRect == annotation::test::pixelRect(0, 0, 2, 1));

        // And the over-broad appearance is still reported for what it is.
        auto const findings = judgeModelCheck(check);
        CHECK_FALSE(findings.empty());
    }

    namespace
    {
        constexpr auto k_closeSource = "00000000-0000-0000-0000-000000000405";
        constexpr auto k_closeMarkId = "00000000-0000-0000-0000-00000000041b";
        constexpr auto k_closePageId = "00000000-0000-0000-0000-000000000426";
        constexpr auto k_closeRegId  = "00000000-0000-0000-0000-000000000435";

        struct ThinSeparationFixture final
        {
            annotation::AuthoringDocument                 document;
            std::vector<annotation::AuthoringSourceAsset> assets{};

            annotation::SourceId  closeSource{annotation::test::sourceId(k_closeSource)};
            annotation::ElementId backId{annotation::test::elementId(k_backId)};
        };

        // A third page whose screen neither appearance was cut from, so its
        // reference pins the one that applies. That is the case the separation
        // rule exists for: the pinned appearance wins there on merit rather than
        // by scoring zero, and the question becomes by how much.
        //
        // close = 20 30 40 50 60 170 180 90
        //
        // "on_dark" (10 20) scores 20 against the 51 ceiling; "on_light"
        // (200 210) scores 60 against the same ceiling. Both outcomes are right
        // -- one hit, one miss -- and the lead is 3x, under the factor of four
        // the repository has measured healthy marks delivering.
        [[nodiscard]]
        auto thinSeparationFixture() -> ThinSeparationFixture
        {
            auto const fingerprint = annotation::test::fingerprint(8, 1, 96, 96);

            auto darkPng  = encodedGreyRow({10, 20, 30, 40, 50, 60, 70, 80});
            auto lightPng = encodedGreyRow({200, 210, 220, 230, 240, 250, 190, 180});
            auto closePng = encodedGreyRow({20, 30, 40, 50, 60, 170, 180, 90});

            auto const darkSource  = annotation::test::sourceId(k_sourceId);
            auto const lightSource = annotation::test::sourceId(k_otherSourceId);
            auto const closeSource = annotation::test::sourceId(k_closeSource);
            auto const backId      = annotation::test::elementId(k_backId);
            auto const darkMarkId  = annotation::test::elementId(k_darkMarkId);
            auto const lightMarkId = annotation::test::elementId(k_lightMarkId);
            auto const closeMarkId = annotation::test::elementId(k_closeMarkId);
            auto const darkPageId  = annotation::test::pageId(k_darkPageId);
            auto const lightPageId = annotation::test::pageId(k_lightPageId);
            auto const closePageId = annotation::test::pageId(k_closePageId);

            auto appearances = std::vector<annotation::Variant>{};
            appearances.emplace_back(
                annotation::test::variant(
                    "on_dark",
                    darkSource,
                    annotation::test::pixelRect(0, 0, 2, 1)
                )
            );
            appearances.emplace_back(
                annotation::test::variant(
                    "on_light",
                    lightSource,
                    annotation::test::pixelRect(0, 0, 2, 1)
                )
            );

            auto document = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {
                    sourceFrom(darkSource, fingerprint, darkPng),
                    sourceFrom(lightSource, fingerprint, lightPng),
                    sourceFrom(closeSource, fingerprint, closePng),
                },
                {
                    test::markElement(
                        fingerprint,
                        darkMarkId,
                        "dark_mark",
                        darkSource,
                        annotation::test::pixelRect(6, 0, 1, 1),
                        annotation::test::pixelRect(6, 0, 1, 1)
                    ),
                    test::markElement(
                        fingerprint,
                        lightMarkId,
                        "light_mark",
                        lightSource,
                        annotation::test::pixelRect(0, 0, 1, 1),
                        annotation::test::pixelRect(0, 0, 1, 1)
                    ),
                    test::markElement(
                        fingerprint,
                        closeMarkId,
                        "close_mark",
                        closeSource,
                        annotation::test::pixelRect(5, 0, 1, 1),
                        annotation::test::pixelRect(5, 0, 1, 1)
                    ),
                    annotation::test::element(
                        fingerprint,
                        backId,
                        "back",
                        annotation::test::capabilities(
                            std::nullopt,
                            annotation::Interact{}
                        ),
                        annotation::test::pixelRect(0, 0, 8, 1),
                        std::move(appearances)
                    ),
                },
                {
                    annotation::test::page(darkPageId, "dark"),
                    annotation::test::page(lightPageId, "light"),
                    annotation::test::page(closePageId, "close"),
                },
                {
                    annotation::test::reference(
                        darkPageId,
                        darkMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        lightPageId,
                        lightMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        closePageId,
                        closeMarkId,
                        annotation::test::identifiesAs()
                    ),
                    annotation::test::reference(
                        darkPageId,
                        backId,
                        annotation::test::interacts()
                    ),
                    annotation::test::reference(
                        lightPageId,
                        backId,
                        annotation::test::interacts(),
                        annotation::Holding::Referenced
                    ),
                    annotation::test::reference(
                        closePageId,
                        backId,
                        annotation::test::interacts(),
                        annotation::Holding::Referenced,
                        std::nullopt,
                        annotation::test::resourceName("on_dark")
                    ),
                },
                {
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_darkRegId),
                            .sourceId = darkSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = darkPageId,
                            },
                        }
                    },
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_lightRegId),
                            .sourceId = lightSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = lightPageId,
                            },
                        }
                    },
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_closeRegId),
                            .sourceId = closeSource,
                            .classification =
                                annotation::RegressionClassification::Positive,
                            .expectation = annotation::ResolvedRegression{
                                .pageId = closePageId,
                            },
                        }
                    },
                }
            );
            REQUIRE(document.has_value());

            return ThinSeparationFixture{
                .document = *std::move(document),
                .assets   = {
                    annotation::AuthoringSourceAsset{
                        .id       = darkSource,
                        .pngBytes = std::move(darkPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = lightSource,
                        .pngBytes = std::move(lightPng),
                    },
                    annotation::AuthoringSourceAsset{
                        .id       = closeSource,
                        .pngBytes = std::move(closePng),
                    },
                },
            };
        }
    }

    TEST_CASE("the owning appearance has to lead its rivals by the separation factor")
    {
        // R3. Every outcome in this model is correct -- the pinned appearance
        // hits its screen and the other one misses it -- so nothing P1, P2 or P3
        // asks can fail. What is wrong is the size of the lead: 3x, where the
        // repository's own healthy marks measured 2.85x to 4.15x. At a factor of
        // one this check degenerates into "the right appearance won", which P3
        // already asserts, and the model below is accepted.
        auto const fixture = thinSeparationFixture();

        auto const check = runModelCheck(
            fixture.document,
            fixture.assets,
            {},
            continuingPolicy(10'000)
        );
        REQUIRE(check.has_value());

        auto const* p_owner = findAppearanceCell(
            *check,
            fixture.backId,
            fixture.closeSource,
            "on_dark"
        );
        REQUIRE(p_owner != nullptr);
        CHECK(p_owner->outcome == ModelCellOutcome::Hit);
        CHECK(p_owner->expectedHit);

        auto const* p_rival = findAppearanceCell(
            *check,
            fixture.backId,
            fixture.closeSource,
            "on_light"
        );
        REQUIRE(p_rival != nullptr);
        CHECK(p_rival->outcome == ModelCellOutcome::Miss);
        CHECK_FALSE(p_rival->expectedHit);

        auto const findings = judgeModelCheck(*check);
        REQUIRE(findings.size() == 1U);
        CHECK(findings.front().kind == ModelFindingKind::ThinSeparation);
        CHECK(findings.front().screenId == fixture.closeSource);
        REQUIRE(findings.front().appearance.has_value());
        CHECK(findings.front().appearance->value() == "on_dark");
        REQUIRE(findings.front().rival.has_value());
        CHECK(findings.front().rival->value() == "on_light");
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
