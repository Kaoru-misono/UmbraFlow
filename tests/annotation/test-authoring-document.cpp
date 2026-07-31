#include "test-helpers.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>
#include <annotation/resource.hpp>

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
        constexpr auto k_secondActionId = "00000000-0000-0000-0000-000000000003";
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
        auto colourKey() -> ColourKey
        {
            auto const key = ColourKey::create(10, 20, 30, 12);
            REQUIRE(key.has_value());
            return *key;
        }

        [[nodiscard]]
        auto authoringDocument() -> AuthoringDocument
        {
            auto const fingerprint = test::fingerprint(8, 6, 96, 96);
            auto const sourceId    = test::sourceId(k_sourceId);
            auto const anchorId    = test::elementId(k_anchorId);
            auto const actionId    = test::elementId(k_actionId);
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
                test::element(
                    fingerprint,
                    actionId,
                    "daily_button",
                    test::capabilities(
                        std::nullopt,
                        Interact{.clickOffset = *click}
                    ),
                    test::pixelRect(3, 2, 4, 3),
                    std::vector<Appearance>{
                        test::appearance(
                            "only",
                            sourceId,
                            test::pixelRect(4, 3, 2, 1),
                            test::threshold(),
                            colourKey()
                        ),
                    }
                )
            );
            elements.emplace_back(
                test::element(
                    fingerprint,
                    anchorId,
                    "home_marker",
                    test::capabilities(Identify{}),
                    test::pixelRect(0, 0, 3, 3),
                    std::vector<Appearance>{
                        test::appearance(
                            "only",
                            sourceId,
                            test::pixelRect(1, 1, 1, 1)
                        ),
                    }
                )
            );

            auto references = std::vector<PageReference>{};
            references.emplace_back(
                test::reference(pageId, anchorId, test::identifiesAs())
            );
            references.emplace_back(
                test::reference(pageId, actionId, test::interacts())
            );

            auto result = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {*source},
                std::move(elements),
                {test::page(pageId, "home")},
                std::move(references),
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
        // Six tables. An annotation row states what the element is and what it
        // may be used for; its pixels live in the appearance rows keyed back to it,
        // because one element can now wear several appearances. A page row is
        // identity alone -- what it requires and forbids is derived from the
        // reference rows, which are also the authorisation, so neither fact is
        // written twice. The colour key stays here and never reaches the runtime
        // manifest: the compiler bakes it into the template's alpha channel.
        auto const expected = std::string{
            "schema = \"umbraflow-authoring/v4\"\n"
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
            "[[element]]\n"
            "id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"home_marker\"\n"
            "search_roi = [0, 0, 3, 3]\n"
            "capabilities = [\"identify\"]\n"
            "\n"
            "[[element]]\n"
            "id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"daily_button\"\n"
            "search_roi = [3, 2, 4, 3]\n"
            "capabilities = [\"interact\"]\n"
            "default_click = [1, 0]\n"
            "\n"
            "[[appearance]]\n"
            "element_id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"only\"\n"
            "source_id = \"00000000-0000-0000-0000-000000000201\"\n"
            "element_kind = \"gray_template\"\n"
            "template_rect = [1, 1, 1, 1]\n"
            "min_similarity_bp = 9000\n"
            "\n"
            "[[appearance]]\n"
            "element_id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"only\"\n"
            "source_id = \"00000000-0000-0000-0000-000000000201\"\n"
            "element_kind = \"gray_template\"\n"
            "template_rect = [4, 3, 2, 1]\n"
            "min_similarity_bp = 9000\n"
            "colour_key = [10, 20, 30]\n"
            "colour_key_tolerance = 12\n"
            "\n"
            "[[page]]\n"
            "id = \"00000000-0000-0000-0000-000000000101\"\n"
            "name = \"home\"\n"
            "\n"
            "[[reference]]\n"
            "page_id = \"00000000-0000-0000-0000-000000000101\"\n"
            "element_id = \"00000000-0000-0000-0000-000000000001\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"identify\"]\n"
            "signature_role = \"required\"\n"
            "\n"
            "[[reference]]\n"
            "page_id = \"00000000-0000-0000-0000-000000000101\"\n"
            "element_id = \"00000000-0000-0000-0000-000000000002\"\n"
            "holding = \"owned\"\n"
            "exercised = [\"interact\"]\n"
            "\n"
            "[[regression]]\n"
            "id = \"00000000-0000-0000-0000-000000000301\"\n"
            "source_id = \"00000000-0000-0000-0000-000000000201\"\n"
            "classification = \"positive\"\n"
            "expected_outcome = \"resolved\"\n"
            "expected_page_id = \"00000000-0000-0000-0000-000000000101\"\n"
        };
        CHECK(encoded == expected);
        // The signature is derived, so the two words it used to be written as
        // must not appear on disk at all.
        CHECK(encoded.find("required = [") == std::string::npos);
        CHECK(encoded.find("forbidden = [") == std::string::npos);

        auto const parsed = parseAuthoringDocument(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeAuthoringDocument(*parsed) == encoded);
        CHECK(parsed->sources().size() == 1U);
        CHECK(parsed->elements().size() == 2U);
        CHECK(parsed->references().size() == 2U);
        CHECK(parsed->catalog().elements().size() == 2U);
        CHECK(parsed->catalog().pages().size() == 1U);
        CHECK(parsed->regressions().size() == 1U);

        auto const* p_action = parsed->findElement(test::elementId(k_actionId));
        REQUIRE(p_action != nullptr);
        REQUIRE(p_action->appearances().size() == 1U);
        CHECK(p_action->appearances().front().colourKey() == colourKey());
    }

    TEST_CASE("annotation authoring reader rejects non-canonical or drifting input")
    {
        auto const canonical = serializeAuthoringDocument(authoringDocument());
        auto invalid         = std::vector<std::string>{};
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
            replaceOnce(canonical, "element_kind = \"gray_template\"", "element_kind = \"raster\"")
        );
        // A colour key and its tolerance are one fact spread over two lines, so
        // half of one must fail rather than default to a value nobody authored.
        invalid.emplace_back(
            replaceOnce(canonical, "colour_key_tolerance = 12\n", "")
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

    TEST_CASE("annotation authoring reader keeps no read path for any retired schema")
    {
        // Every retired version, not only the one just replaced: v1 once had a
        // documented upgrade path in the reader, and this is what says that path
        // left with it rather than surviving one bump behind.
        auto const canonical = serializeAuthoringDocument(authoringDocument());
        for (
            auto const retired : {
                std::string_view{"umbraflow-authoring/v1"},
                std::string_view{"umbraflow-authoring/v2"},
                std::string_view{"umbraflow-authoring/v3"},
            }
        )
        {
            INFO(retired);
            auto const drifted = replaceOnce(
                canonical,
                "umbraflow-authoring/v4",
                retired
            );
            auto const rejected = parseAuthoringDocument(drifted);
            REQUIRE_FALSE(rejected.has_value());
            test::requireErrorKind(
                rejected.error(),
                AutomationErrorKind::InvalidResource
            );
            CHECK(rejected.error().message().contains("unsupported"));
            CHECK(rejected.error().message().contains("schema"));
        }
    }

    TEST_CASE("annotation authoring references serialize in canonical page then element order")
    {
        auto const fingerprint  = test::fingerprint(8, 6, 96, 96);
        auto const sourceId     = test::sourceId(k_sourceId);
        auto const anchorId     = test::elementId(k_anchorId);
        auto const lowerId      = test::elementId(k_actionId);
        auto const higherId     = test::elementId(k_secondActionId);
        auto const pageId       = test::pageId(k_pageId);
        auto const roi          = test::pixelRect(0, 0, 3, 3);
        auto const templateRect = test::pixelRect(0, 0, 1, 1);

        auto const interactive = [&](ElementId id, std::string name)
        {
            return test::element(
                fingerprint,
                id,
                std::move(name),
                test::capabilities(std::nullopt, Interact{}),
                roi,
                std::vector<Appearance>{
                    test::appearance("only", sourceId, templateRect),
                }
            );
        };

        auto elements = std::vector<Element>{};
        elements.emplace_back(
            test::element(
                fingerprint,
                anchorId,
                "home_marker",
                test::capabilities(Identify{}),
                roi,
                std::vector<Appearance>{
                    test::appearance("only", sourceId, templateRect),
                }
            )
        );
        elements.emplace_back(interactive(higherId, "second_button"));
        elements.emplace_back(interactive(lowerId, "first_button"));

        // Supplied out of order; canonical output must sort them by element id.
        auto references = std::vector<PageReference>{};
        references.emplace_back(
            test::reference(pageId, higherId, test::interacts())
        );
        references.emplace_back(
            test::reference(pageId, lowerId, test::interacts())
        );
        references.emplace_back(
            test::reference(pageId, anchorId, test::identifiesAs())
        );

        auto document = AuthoringDocument::create(
            test::projectId(),
            fingerprint,
            {importedSource(sourceId, fingerprint)},
            std::move(elements),
            {test::page(pageId, "home")},
            std::move(references),
            {}
        );
        REQUIRE(document.has_value());

        auto const encoded = serializeAuthoringDocument(*document);
        // Searched from the first reference table, because the appearance rows
        // above carry element_id too and are sorted by the same key: matching
        // them would let this case pass without the references being ordered.
        auto const firstReference = encoded.find("\n[[reference]]\n");
        REQUIRE(firstReference != std::string::npos);
        auto const positionOf = [&encoded, firstReference](char const* id)
        {
            return encoded.find(
                std::string{"element_id = \""} + id + '"',
                firstReference
            );
        };
        auto const anchorPos = positionOf(k_anchorId);
        auto const lowerPos  = positionOf(k_actionId);
        auto const higherPos = positionOf(k_secondActionId);
        REQUIRE(anchorPos != std::string::npos);
        REQUIRE(lowerPos != std::string::npos);
        REQUIRE(higherPos != std::string::npos);
        CHECK(anchorPos < lowerPos);
        CHECK(lowerPos < higherPos);

        auto const parsed = parseAuthoringDocument(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeAuthoringDocument(*parsed) == encoded);
        CHECK(parsed->references().size() == 3U);
    }
}
