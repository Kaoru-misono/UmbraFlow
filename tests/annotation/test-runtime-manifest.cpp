#include "test-helpers.hpp"

#include <annotation/runtime-manifest.hpp>

#include <doctest/doctest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_pageId = "00000000-0000-0000-0000-000000000101";
        constexpr auto k_anchorHash =
            "sha256:11111111111111111111111111111111"
            "11111111111111111111111111111111";
        constexpr auto k_actionHash =
            "sha256:22222222222222222222222222222222"
            "22222222222222222222222222222222";
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
        auto runtimeManifest(std::string project = "personal.test") -> RuntimeManifest
        {
            auto const fingerprint = test::fingerprint(8, 6, 96, 96);
            auto const anchorId = test::elementId(k_anchorId);
            auto const actionId = test::elementId(k_actionId);
            auto const pageId = test::pageId(k_pageId);
            auto const click = TemplateOffset::create(1, 0, 2, 1);
            REQUIRE(click.has_value());

            auto result = RuntimeManifest::create(
                test::projectId(std::move(project)),
                fingerprint,
                {
                    RuntimeRecognizerSpec{
                        .definition = test::recognizer(
                            fingerprint,
                            actionId,
                            "daily_button",
                            AnnotationType::ActionTarget,
                            test::pixelRect(4, 3, 2, 1),
                            test::pixelRect(3, 2, 4, 3),
                            {pageId},
                            *click
                        ),
                        .templateHash = contentHash(k_actionHash),
                        .sourceHash   = contentHash(k_sourceHash),
                    },
                    RuntimeRecognizerSpec{
                        .definition = test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            AnnotationType::PageAnchor,
                            test::pixelRect(1, 1, 1, 1),
                            test::pixelRect(0, 0, 3, 3)
                        ),
                        .templateHash = contentHash(k_anchorHash),
                        .sourceHash   = contentHash(k_sourceHash),
                    },
                },
                {test::page(pageId, "home", {anchorId})}
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

    TEST_CASE("annotation runtime manifest compilation is byte-stable and runtime-only")
    {
        auto const encoded = serializeRuntimeManifest(runtimeManifest());
        auto const expected = std::string{
            "schema = \"umbraflow-annotations/v1\"\n"
            "project_id = \"personal.test\"\n"
            "base_resolution = [8, 6]\n"
            "base_dpi = [96, 96]\n"
            "\n"
            "[[recognizer]]\n"
            "id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"home_marker\"\n"
            "annotation_type = \"page_anchor\"\n"
            "kind = \"gray_template\"\n"
            "template = \"assets/templates/1111111111111111111111111111111111111111111111111111111111111111.png\"\n"
            "template_hash = \"sha256:1111111111111111111111111111111111111111111111111111111111111111\"\n"
            "source_hash = \"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "template_rect = [1, 1, 1, 1]\n"
            "search_roi = [0, 0, 3, 3]\n"
            "min_similarity_bp = 9000\n"
            "\n"
            "[[recognizer]]\n"
            "id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"daily_button\"\n"
            "annotation_type = \"action_target\"\n"
            "kind = \"gray_template\"\n"
            "template = \"assets/templates/2222222222222222222222222222222222222222222222222222222222222222.png\"\n"
            "template_hash = \"sha256:2222222222222222222222222222222222222222222222222222222222222222\"\n"
            "source_hash = \"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "template_rect = [4, 3, 2, 1]\n"
            "search_roi = [3, 2, 4, 3]\n"
            "min_similarity_bp = 9000\n"
            "default_click = [1, 0]\n"
            "allowed_page_ids = [\"00000000-0000-0000-0000-000000000101\"]\n"
            "\n"
            "[[page]]\n"
            "id = \"00000000-0000-0000-0000-000000000101\"\n"
            "name = \"home\"\n"
            "required = [\"00000000-0000-0000-0000-000000000001\"]\n"
            "forbidden = []\n"
        };
        CHECK(encoded == expected);
        CHECK(encoded.find("source_id") == std::string::npos);
        CHECK(encoded.find("captured_at") == std::string::npos);
        CHECK(encoded.find("target_generation") == std::string::npos);

        auto const parsed = parseRuntimeManifest(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeRuntimeManifest(*parsed) == encoded);
        CHECK(parsed->catalog().recognizers().size() == 2U);
        CHECK(parsed->catalog().pages().size() == 1U);
        auto const* p_asset = parsed->findAsset(test::elementId(k_anchorId));
        REQUIRE(p_asset != nullptr);
        CHECK(p_asset->templateHash == contentHash(k_anchorHash));
    }

    TEST_CASE("annotation runtime manifest preserves canonical TOML string escapes")
    {
        auto const encoded = serializeRuntimeManifest(
            runtimeManifest("personal.\"quoted\"\\line\nnext")
        );
        CHECK(
            encoded.starts_with(
                "schema = \"umbraflow-annotations/v1\"\n"
                "project_id = \"personal.\\\"quoted\\\"\\\\line\\nnext\"\n"
            )
        );
        auto const parsed = parseRuntimeManifest(encoded);
        REQUIRE(parsed.has_value());
        CHECK(
            parsed->catalog().projectId().value()
            == "personal.\"quoted\"\\line\nnext"
        );
    }

    TEST_CASE("annotation runtime manifest reader rejects non-canonical or drifting input")
    {
        auto const canonical = serializeRuntimeManifest(runtimeManifest());
        auto invalid = std::vector<std::string>{};
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "umbraflow-annotations/v1",
                "umbraflow-annotations/v2"
            )
        );
        invalid.emplace_back(
            replaceOnce(canonical, "min_similarity_bp = 9000", "min_similarity_bp = 09000")
        );
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "assets/templates/1111",
                "assets/templates/0111"
            )
        );
        invalid.emplace_back(
            replaceOnce(canonical, "kind = \"gray_template\"", "kind = \"color\"")
        );
        invalid.emplace_back(
            replaceOnce(canonical, "\n[[page]]", "\nunknown = 1\n\n[[page]]")
        );
        invalid.emplace_back(
            replaceOnce(canonical, "\n", "\r\n")
        );
        invalid.emplace_back(canonical + "# generated files accept no trailing comments\n");

        for (auto const& text : invalid)
        {
            auto const rejected = parseRuntimeManifest(text);
            REQUIRE_FALSE(rejected.has_value());
            test::requireErrorKind(
                rejected.error(),
                AutomationErrorKind::InvalidResource
            );
        }
    }
}
