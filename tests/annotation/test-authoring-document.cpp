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
        constexpr auto g_sourceId     = "00000000-0000-0000-0000-000000000201";
        constexpr auto g_anchorId     = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_actionId     = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_pageId       = "00000000-0000-0000-0000-000000000101";
        constexpr auto g_regressionId = "00000000-0000-0000-0000-000000000301";
        constexpr auto g_sourceHash =
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
        auto authoringDocument() -> AuthoringDocument
        {
            auto const fingerprint = test::fingerprint(8, 6, 96, 96);
            auto const sourceId    = test::sourceId(g_sourceId);
            auto const anchorId    = test::recognizerId(g_anchorId);
            auto const actionId    = test::recognizerId(g_actionId);
            auto const pageId      = test::pageId(g_pageId);
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = contentHash(g_sourceHash),
                    .m_fingerprint = fingerprint,
                    .m_provenance  = WgcSourceProvenance{
                        .m_targetGeneration = TargetGeneration::fromValue(7),
                        .m_capturedAt       = "2026-07-23T09:15:00+09:00",
                    },
                }
            );
            REQUIRE(source.has_value());
            auto const click = TemplateOffset::create(1, 0, 2, 1);
            REQUIRE(click.has_value());
            auto regression = RegressionCase{
                RegressionSpec{
                    .m_id             = test::regressionId(g_regressionId),
                    .m_sourceId       = sourceId,
                    .m_classification = RegressionClassification::Positive,
                    .m_expectation    = ResolvedRegression{pageId},
                }
            };

            auto result = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {*source},
                {
                    AuthoringRecognizerSpec{
                        .m_definition = test::recognizer(
                            fingerprint,
                            actionId,
                            "daily_button",
                            AnnotationType::ActionTarget,
                            test::pixelRect(4, 3, 2, 1),
                            test::pixelRect(3, 2, 4, 3),
                            {pageId},
                            *click
                        ),
                        .m_sourceId = sourceId,
                    },
                    AuthoringRecognizerSpec{
                        .m_definition = test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            AnnotationType::PageAnchor,
                            test::pixelRect(1, 1, 1, 1),
                            test::pixelRect(0, 0, 3, 3)
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {test::page(pageId, "home", {anchorId})},
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
            "schema = \"umbraflow-authoring/v1\"\n"
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
            "page_ids = [\"00000000-0000-0000-0000-000000000101\"]\n"
            "\n"
            "[[page]]\n"
            "id = \"00000000-0000-0000-0000-000000000101\"\n"
            "name = \"home\"\n"
            "required = [\"00000000-0000-0000-0000-000000000001\"]\n"
            "forbidden = []\n"
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
                "umbraflow-authoring/v1",
                "umbraflow-authoring/v2"
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
                std::string{"id = \""} + g_regressionId + '"',
                std::string{"id = \""} + g_sourceId + '"'
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
            RegressionClassification  m_classification{};
            std::string_view          m_text{};
        };
        constexpr auto cases = std::array{
            ClassificationCase{
                .m_classification = RegressionClassification::Positive,
                .m_text           = "positive",
            },
            ClassificationCase{
                .m_classification = RegressionClassification::Negative,
                .m_text           = "negative",
            },
            ClassificationCase{
                .m_classification = RegressionClassification::Confusable,
                .m_text           = "confusable",
            }
        };

        auto const canonical = serializeAuthoringDocument(authoringDocument());
        for (auto const& entry : cases)
        {
            auto const encoded = replaceOnce(
                canonical,
                "classification = \"positive\"",
                std::string{"classification = \""} + std::string{entry.m_text} + '"'
            );
            auto const parsed = parseAuthoringDocument(encoded);
            REQUIRE(parsed.has_value());
            REQUIRE(parsed->regressions().size() == 1U);
            auto const& regression = parsed->regressions().front();
            CHECK(regression.classification() == entry.m_classification);
            auto const* p_resolved = std::get_if<ResolvedRegression>(
                &regression.expectation()
            );
            REQUIRE(p_resolved != nullptr);
            CHECK(p_resolved->m_pageId == test::pageId(g_pageId));
            CHECK(serializeAuthoringDocument(*parsed) == encoded);
        }
    }
}
