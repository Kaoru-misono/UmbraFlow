#include "test-helpers.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_sourceId     = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_anchorId     = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId     = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_pageId       = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_regressionId = "00000000-0000-0000-0000-000000000301";
        constexpr auto k_sourceHash =
            "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

        [[nodiscard]]
        auto contentHash(std::string_view value) -> ContentHash
        {
            auto const parsed = ContentHash::parse(value);
            REQUIRE(parsed.has_value());
            return *parsed;
        }

        [[nodiscard]]
        auto importedSource(
            SourceId id,
            ProjectFingerprint fingerprint
        ) -> AuthoringSource
        {
            auto result = AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = id,
                    .contentHash = contentHash(k_sourceHash),
                    .fingerprint = fingerprint,
                    .provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        [[nodiscard]]
        auto authoringDocument() -> AuthoringDocument
        {
            auto const fingerprint = test::fingerprint(8, 6, 96, 96);
            auto const sourceId    = test::sourceId(k_sourceId);
            auto const anchorId    = test::recognizerId(k_anchorId);
            auto const actionId    = test::recognizerId(k_actionId);
            auto const pageId      = test::pageId(k_pageId);
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = contentHash(k_sourceHash),
                    .fingerprint = fingerprint,
                    .provenance  = WgcSourceProvenance{
                        .targetGeneration = TargetGeneration::fromValue(7),
                        .capturedAt       = "2026-07-23T09:15:00+09:00",
                    },
                }
            );
            REQUIRE(source.has_value());
            auto const click = TemplateOffset::create(1, 0, 2, 1);
            REQUIRE(click.has_value());
            auto regression = RegressionCase{
                RegressionSpec{
                    .id             = test::regressionId(k_regressionId),
                    .sourceId       = sourceId,
                    .classification = RegressionClassification::Positive,
                    .expectation    = ResolvedRegression{pageId},
                }
            };

            auto elements = std::vector<Element>{};
            elements.emplace_back(
                test::interactiveElement(
                    fingerprint,
                    actionId,
                    "daily_button",
                    sourceId,
                    test::pixelRect(4, 3, 2, 1),
                    test::pixelRect(3, 2, 4, 3),
                    *click
                )
            );
            elements.emplace_back(
                test::anchorElement(
                    fingerprint,
                    anchorId,
                    "home_marker",
                    sourceId,
                    test::pixelRect(1, 1, 1, 1),
                    test::pixelRect(0, 0, 3, 3)
                )
            );

            auto result = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {*source},
                std::move(elements),
                {test::page(pageId, "home", {anchorId})},
                {test::placement(pageId, actionId, test::pixelRect(3, 2, 4, 3))},
                {regression}
            );
            REQUIRE(result.has_value());
            return *std::move(result);
        }

        [[nodiscard]]
        auto replaceOnce(
            std::string source,
            std::string_view from,
            std::string_view to
        ) -> std::string
        {
            auto const position = source.find(from);
            REQUIRE(position != std::string::npos);
            source.replace(position, from.size(), to);
            return source;
        }
    }

    TEST_CASE("annotation authoring document has a byte-stable complete round trip")
    {
        auto const encoded = serializeAuthoringDocument(authoringDocument());
        auto const expected = std::string{
            "schema = \"umbraflow-authoring/v2\"\n"
            "project_id = \"personal.test\"\n"
            "base_resolution = [8, 6]\n"
            "base_dpi = [96, 96]\n"
            "\n"
            "[[source]]\n"
            "id = \"00000000-0000-0000-0000-000000000201\"\n"
            "path = \"assets/sources/"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.png\"\n"
            "content_hash = \"sha256:"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "client_size = [8, 6]\n"
            "dpi = [96, 96]\n"
            "capture_backend = \"wgc\"\n"
            "target_generation = 7\n"
            "captured_at = \"2026-07-23T09:15:00+09:00\"\n"
            "\n"
            "[[annotation]]\n"
            "id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"home_marker\"\n"
            "type = \"page_anchor\"\n"
            "source_id = \"00000000-0000-0000-0000-000000000201\"\n"
            "recognizer_kind = \"gray_template\"\n"
            "template_rect = [1, 1, 1, 1]\n"
            "search_roi = [0, 0, 3, 3]\n"
            "min_similarity_bp = 9000\n"
            "\n"
            "[[annotation]]\n"
            "id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"daily_button\"\n"
            "type = \"action_target\"\n"
            "source_id = \"00000000-0000-0000-0000-000000000201\"\n"
            "recognizer_kind = \"gray_template\"\n"
            "template_rect = [4, 3, 2, 1]\n"
            "search_roi = [3, 2, 4, 3]\n"
            "min_similarity_bp = 9000\n"
            "default_click = [1, 0]\n"
            "\n"
            "[[page]]\n"
            "id = \"00000000-0000-0000-0000-000000000101\"\n"
            "name = \"home\"\n"
            "required = [\"00000000-0000-0000-0000-000000000001\"]\n"
            "forbidden = []\n"
            "\n"
            "[[placement]]\n"
            "page_id = \"00000000-0000-0000-0000-000000000101\"\n"
            "element_id = \"00000000-0000-0000-0000-000000000002\"\n"
            "search_roi = [3, 2, 4, 3]\n"
            "\n"
            "[[regression]]\n"
            "id = \"00000000-0000-0000-0000-000000000301\"\n"
            "source_id = \"00000000-0000-0000-0000-000000000201\"\n"
            "classification = \"positive\"\n"
            "expected_outcome = \"resolved\"\n"
            "expected_page_id = \"00000000-0000-0000-0000-000000000101\"\n"
        };
        CHECK(encoded == expected);

        auto const parsed = parseAuthoringDocument(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeAuthoringDocument(*parsed) == encoded);
        CHECK(parsed->sources().size() == 1U);
        CHECK(parsed->catalog().recognizers().size() == 2U);
        CHECK(parsed->catalog().pages().size() == 1U);
        CHECK(parsed->regressions().size() == 1U);
    }

    TEST_CASE("annotation authoring reader rejects non-canonical or drifting input")
    {
        auto const canonical = serializeAuthoringDocument(authoringDocument());
        auto invalid         = std::vector<std::string>{};
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "umbraflow-authoring/v2",
                "umbraflow-authoring/v3"
            )
        );
        invalid.emplace_back(
            replaceOnce(canonical, "target_generation = 7", "target_generation = 07")
        );
        invalid.emplace_back(
            replaceOnce(canonical, "assets/sources/aaaa", "assets/sources/baaa")
        );
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "source_id = \"00000000-0000-0000-0000-000000000201\"",
                "source_id = \"00000000-0000-0000-0000-000000000202\""
            )
        );
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "captured_at = \"2026-07-23T09:15:00+09:00\"",
                "captured_at = \"2026-02-30T09:15:00+09:00\""
            )
        );
        invalid.emplace_back(
            replaceOnce(canonical, "\n[[page]]", "\nunknown = 1\n\n[[page]]")
        );
        invalid.emplace_back(
            replaceOnce(
                canonical,
                std::string{"id = \""} + k_regressionId + '"',
                std::string{"id = \""} + k_sourceId + '"'
            )
        );
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "template_rect = [1, 1, 1, 1]",
                "template_rect = [1, 1, 0, 1]"
            )
        );
        invalid.emplace_back(replaceOnce(canonical, "\n", "\r\n"));

        for (auto const& text : invalid)
        {
            auto const rejected = parseAuthoringDocument(text);
            REQUIRE_FALSE(rejected.has_value());
            test::requireErrorKind(
                rejected.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }

    TEST_CASE("annotation regression classification preserves its independent outcome")
    {
        struct ClassificationCase final
        {
            RegressionClassification  classification{};
            std::string_view          text{};
        };
        constexpr auto cases = std::array{
            ClassificationCase{
                .classification = RegressionClassification::Positive,
                .text           = "positive",
            },
            ClassificationCase{
                .classification = RegressionClassification::Negative,
                .text           = "negative",
            },
            ClassificationCase{
                .classification = RegressionClassification::Confusable,
                .text           = "confusable",
            }
        };

        auto const canonical = serializeAuthoringDocument(authoringDocument());
        for (auto const& entry : cases)
        {
            auto const encoded = replaceOnce(
                canonical,
                "classification = \"positive\"",
                std::string{"classification = \""} + std::string{entry.text} + '"'
            );
            auto const parsed = parseAuthoringDocument(encoded);
            REQUIRE(parsed.has_value());
            REQUIRE(parsed->regressions().size() == 1U);
            auto const& regression = parsed->regressions().front();
            CHECK(regression.classification() == entry.classification);
            auto const* p_resolved = std::get_if<ResolvedRegression>(
                &regression.expectation()
            );
            REQUIRE(p_resolved != nullptr);
            CHECK(p_resolved->pageId == test::pageId(k_pageId));
            CHECK(serializeAuthoringDocument(*parsed) == encoded);
        }
    }

    TEST_CASE("annotation authoring reader rejects a retired v1 schema string")
    {
        // The v1 read path is gone: the only real project migrated to v2 on disk,
        // so an old schema string now fails as an ordinary unsupported schema
        // rather than upgrading.
        auto const canonical = serializeAuthoringDocument(authoringDocument());
        auto const drifted   = replaceOnce(
            canonical,
            "umbraflow-authoring/v2",
            "umbraflow-authoring/v1"
        );
        auto const rejected = parseAuthoringDocument(drifted);
        REQUIRE_FALSE(rejected.has_value());
        CHECK(rejected.error().message().contains("unsupported"));
    }

    TEST_CASE("annotation authoring placements serialize in canonical page then element order")
    {
        auto const fingerprint = test::fingerprint(8, 6, 96, 96);
        auto const sourceId    = test::sourceId(k_sourceId);
        auto const anchorId    = test::recognizerId(k_anchorId);
        auto const lowerId     = test::recognizerId(
            "00000000-0000-0000-0000-000000000002"
        );
        auto const higherId    = test::recognizerId(
            "00000000-0000-0000-0000-000000000003"
        );
        auto const pageId      = test::pageId(k_pageId);
        auto const roi         = test::pixelRect(0, 0, 3, 3);
        auto const templateRect = test::pixelRect(0, 0, 1, 1);

        auto elements = std::vector<Element>{};
        elements.emplace_back(
            test::anchorElement(
                fingerprint,
                anchorId,
                "home_marker",
                sourceId,
                templateRect,
                roi
            )
        );
        elements.emplace_back(
            test::interactiveElement(
                fingerprint,
                higherId,
                "second_button",
                sourceId,
                templateRect,
                roi
            )
        );
        elements.emplace_back(
            test::interactiveElement(
                fingerprint,
                lowerId,
                "first_button",
                sourceId,
                templateRect,
                roi
            )
        );

        // Supplied out of order; canonical output must sort them by element id.
        auto placements = std::vector<AuthoringPlacement>{
            test::placement(pageId, higherId, roi),
            test::placement(pageId, lowerId, roi),
        };

        auto document = AuthoringDocument::create(
            test::projectId(),
            fingerprint,
            {importedSource(sourceId, fingerprint)},
            std::move(elements),
            {test::page(pageId, "home", {anchorId})},
            std::move(placements),
            {}
        );
        REQUIRE(document.has_value());

        auto const encoded  = serializeAuthoringDocument(*document);
        auto const lowerPos = encoded.find(
            "element_id = \"00000000-0000-0000-0000-000000000002\""
        );
        auto const higherPos = encoded.find(
            "element_id = \"00000000-0000-0000-0000-000000000003\""
        );
        REQUIRE(lowerPos != std::string::npos);
        REQUIRE(higherPos != std::string::npos);
        CHECK(lowerPos < higherPos);

        auto const parsed = parseAuthoringDocument(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeAuthoringDocument(*parsed) == encoded);
        CHECK(parsed->placements().size() == 2U);
    }

    TEST_CASE("annotation authoring rejects the placements model's new violations")
    {
        auto const fingerprint = test::fingerprint(8, 6, 96, 96);
        auto const sourceId    = test::sourceId(k_sourceId);
        auto const anchorId    = test::recognizerId(k_anchorId);
        auto const regionId    = test::recognizerId(k_actionId);
        auto const pageId      = test::pageId(k_pageId);
        auto const roi         = test::pixelRect(0, 0, 3, 3);
        auto const templateRect = test::pixelRect(0, 0, 1, 1);

        auto const anchor = [&]
        {
            return test::anchorElement(
                fingerprint,
                anchorId,
                "home_marker",
                sourceId,
                templateRect,
                roi
            );
        };
        auto const region = [&]
        {
            return test::interactiveElement(
                fingerprint,
                regionId,
                "daily_button",
                sourceId,
                templateRect,
                roi
            );
        };

        SUBCASE("a placement may not reference a page anchor")
        {
            auto elements = std::vector<Element>{};
            elements.emplace_back(anchor());
            elements.emplace_back(region());
            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {importedSource(sourceId, fingerprint)},
                std::move(elements),
                {test::page(pageId, "home", {anchorId})},
                {
                    test::placement(pageId, anchorId, roi),
                    test::placement(pageId, regionId, roi),
                },
                {}
            );
            REQUIRE_FALSE(document.has_value());
            CHECK(document.error().message().contains("is a page anchor"));
        }

        SUBCASE("an element may not be a signature member and a placement on one page")
        {
            // A page whose signature names the interactive element itself.
            auto elements = std::vector<Element>{};
            elements.emplace_back(anchor());
            elements.emplace_back(region());
            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {importedSource(sourceId, fingerprint)},
                std::move(elements),
                {test::page(pageId, "home", {anchorId, regionId})},
                {test::placement(pageId, regionId, roi)},
                {}
            );
            REQUIRE_FALSE(document.has_value());
            CHECK(
                document.error().message().contains(
                    "both a signature member and a placement"
                )
            );
        }

        SUBCASE("every interactive element must be placed somewhere")
        {
            auto elements = std::vector<Element>{};
            elements.emplace_back(anchor());
            elements.emplace_back(region());
            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {importedSource(sourceId, fingerprint)},
                std::move(elements),
                {test::page(pageId, "home", {anchorId})},
                {},
                {}
            );
            REQUIRE_FALSE(document.has_value());
            CHECK(
                document.error().message().contains(
                    "must be placed on at least one page"
                )
            );
        }
    }
}
