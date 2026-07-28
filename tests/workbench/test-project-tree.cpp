#include "../annotation/test-helpers.hpp"

#include <project-tree.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>
#include <annotation/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        // Six screens, one per bucket state, plus two that resolve to one page so
        // its regression list is genuinely a list. Distinct marker bytes give each
        // source a distinct content hash.
        constexpr auto k_screenAId  = "00000000-0000-0000-0000-0000000000a1";
        constexpr auto k_screenA2Id = "00000000-0000-0000-0000-0000000000a2";
        constexpr auto k_screenBId  = "00000000-0000-0000-0000-0000000000b1";
        constexpr auto k_screenCId  = "00000000-0000-0000-0000-0000000000c1";
        constexpr auto k_screenDId  = "00000000-0000-0000-0000-0000000000d1";
        constexpr auto k_screenQId  = "00000000-0000-0000-0000-0000000000e1";

        constexpr auto k_anchorPId = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_anchorQId = "00000000-0000-0000-0000-000000000002";

        constexpr auto k_pageP = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_pageQ = "00000000-0000-0000-0000-000000000102";

        constexpr auto k_regA  = "00000000-0000-0000-0000-000000000301";
        constexpr auto k_regA2 = "00000000-0000-0000-0000-000000000302";
        constexpr auto k_regB  = "00000000-0000-0000-0000-000000000303";
        constexpr auto k_regC  = "00000000-0000-0000-0000-000000000304";
        constexpr auto k_regQ  = "00000000-0000-0000-0000-000000000305";

        [[nodiscard]]
        auto makeSource(
            std::string_view idText,
            annotation::ProjectFingerprint fingerprint,
            std::byte marker
        ) -> annotation::AuthoringSource
        {
            auto const bytes = std::array<std::byte, 1>{marker};
            auto const hash  = annotation::sha256(std::span<std::byte const>{bytes});
            REQUIRE(hash.has_value());
            auto source = annotation::AuthoringSource::create(
                annotation::AuthoringSourceSpec{
                    .id          = annotation::test::sourceId(idText),
                    .contentHash = *hash,
                    .fingerprint = fingerprint,
                    .provenance  = annotation::ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            return *std::move(source);
        }

        [[nodiscard]]
        auto resolvedCase(
            std::string_view regId,
            std::string_view srcId,
            std::string_view pageId
        ) -> annotation::RegressionCase
        {
            return annotation::RegressionCase{
                annotation::RegressionSpec{
                    .id             = annotation::test::regressionId(regId),
                    .sourceId       = annotation::test::sourceId(srcId),
                    .classification = annotation::RegressionClassification::Positive,
                    .expectation    = annotation::ResolvedRegression{
                        .pageId = annotation::test::pageId(pageId),
                    },
                }
            };
        }

        // Sources are declared A, A2, B, C, D, Q so a bucket's document order is
        // asserted rather than assumed. A and A2 resolve to page P, Q to page Q,
        // B is unknown, C ambiguous, D carries no case at all.
        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const anchorPId   = annotation::test::recognizerId(k_anchorPId);
            auto const anchorQId   = annotation::test::recognizerId(k_anchorQId);
            auto const pageP       = annotation::test::pageId(k_pageP);

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {
                    makeSource(k_screenAId, fingerprint, std::byte{0x0A}),
                    makeSource(k_screenA2Id, fingerprint, std::byte{0x1A}),
                    makeSource(k_screenBId, fingerprint, std::byte{0x0B}),
                    makeSource(k_screenCId, fingerprint, std::byte{0x0C}),
                    makeSource(k_screenDId, fingerprint, std::byte{0x0D}),
                    makeSource(k_screenQId, fingerprint, std::byte{0x0E}),
                },
                {
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorPId,
                        "home_marker",
                        annotation::test::sourceId(k_screenAId),
                        annotation::test::pixelRect(0, 0, 2, 2),
                        annotation::test::pixelRect(0, 0, 4, 4)
                    ),
                    annotation::test::anchorElement(
                        fingerprint,
                        anchorQId,
                        "battle_marker",
                        annotation::test::sourceId(k_screenQId),
                        annotation::test::pixelRect(4, 4, 2, 2),
                        annotation::test::pixelRect(3, 3, 4, 4)
                    ),
                },
                {
                    annotation::test::page(pageP, "home", {anchorPId}),
                    annotation::test::page(
                        annotation::test::pageId(k_pageQ),
                        "battle",
                        {anchorQId},
                        {anchorPId}
                    ),
                },
                {},
                {
                    resolvedCase(k_regA, k_screenAId, k_pageP),
                    resolvedCase(k_regA2, k_screenA2Id, k_pageP),
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_regB),
                            .sourceId = annotation::test::sourceId(k_screenBId),
                            .classification =
                                annotation::RegressionClassification::Negative,
                            .expectation = annotation::UnknownRegression{},
                        }
                    },
                    annotation::RegressionCase{
                        annotation::RegressionSpec{
                            .id       = annotation::test::regressionId(k_regC),
                            .sourceId = annotation::test::sourceId(k_screenCId),
                            .classification =
                                annotation::RegressionClassification::Confusable,
                            .expectation = annotation::AmbiguousRegression{},
                        }
                    },
                    resolvedCase(k_regQ, k_screenQId, k_pageQ),
                }
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }
    }

    TEST_CASE("a screen's bucket follows its regression case")
    {
        auto const doc = document();

        // No case at all is the true to-do.
        CHECK(
            screenBucketOf(doc, annotation::test::sourceId(k_screenDId))
            == ScreenBucket::NeedsClassification
        );
        CHECK(
            screenBucketOf(doc, annotation::test::sourceId(k_screenBId))
            == ScreenBucket::ExpectedUnknown
        );
        CHECK(
            screenBucketOf(doc, annotation::test::sourceId(k_screenCId))
            == ScreenBucket::ExpectedAmbiguous
        );
        // A screen resolving to a page is owned by that page, not a bucket.
        CHECK(
            screenBucketOf(doc, annotation::test::sourceId(k_screenAId))
            == ScreenBucket::Resolved
        );
    }

    TEST_CASE("each bucket lists its screens in document order")
    {
        auto const doc = document();

        CHECK(
            screensInBucket(doc, ScreenBucket::NeedsClassification)
            == std::vector<annotation::SourceId>{
                annotation::test::sourceId(k_screenDId),
            }
        );
        CHECK(
            screensInBucket(doc, ScreenBucket::ExpectedUnknown)
            == std::vector<annotation::SourceId>{
                annotation::test::sourceId(k_screenBId),
            }
        );
        CHECK(
            screensInBucket(doc, ScreenBucket::ExpectedAmbiguous)
            == std::vector<annotation::SourceId>{
                annotation::test::sourceId(k_screenCId),
            }
        );
        // Resolved is every page-owned screen, across every page.
        CHECK(
            screensInBucket(doc, ScreenBucket::Resolved)
            == std::vector<annotation::SourceId>{
                annotation::test::sourceId(k_screenAId),
                annotation::test::sourceId(k_screenA2Id),
                annotation::test::sourceId(k_screenQId),
            }
        );
    }

    TEST_CASE("a page's regression screens are every screen resolving to it")
    {
        auto const doc = document();

        // Page P owns two screens; the list is not the single first-match claim.
        CHECK(
            regressionScreensForPage(doc, annotation::test::pageId(k_pageP))
            == std::vector<annotation::SourceId>{
                annotation::test::sourceId(k_screenAId),
                annotation::test::sourceId(k_screenA2Id),
            }
        );
        CHECK(
            regressionScreensForPage(doc, annotation::test::pageId(k_pageQ))
            == std::vector<annotation::SourceId>{
                annotation::test::sourceId(k_screenQId),
            }
        );
    }
}
