#include "../annotation/test-helpers.hpp"

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
