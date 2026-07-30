#include "test-helpers.hpp"

#include <annotation/runtime-manifest.hpp>

#include <doctest/doctest.h>

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

            auto recognizers = std::vector<RuntimeRecognizerSpec>{};
            recognizers.emplace_back(
                RuntimeRecognizerSpec{
                    .definition = test::recognizer(
                        fingerprint,
                        actionId,
                        "daily_button",
                        test::capabilities(
                            std::nullopt,
                            Interact{.clickOffset = *click}
                        ),
                        test::pixelRect(3, 2, 4, 3),
                        std::vector<RecognizerVariant>{
                            test::recognizerVariant(
                                "only",
                                test::pixelRect(4, 3, 2, 1)
                            ),
                        }
                    ),
                    .variants = std::vector<RuntimeVariantAsset>{
                        RuntimeVariantAsset{
                            .variantName  = test::resourceName("only"),
                            .templateHash = contentHash(k_actionHash),
                            .sourceHash   = contentHash(k_sourceHash),
                        },
                    },
                }
            );
            recognizers.emplace_back(
                RuntimeRecognizerSpec{
                    .definition = test::recognizer(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        test::capabilities(Identify{}),
                        test::pixelRect(0, 0, 3, 3),
                        std::vector<RecognizerVariant>{
                            test::recognizerVariant(
                                "only",
                                test::pixelRect(1, 1, 1, 1)
                            ),
                        }
                    ),
                    .variants = std::vector<RuntimeVariantAsset>{
                        RuntimeVariantAsset{
                            .variantName  = test::resourceName("only"),
                            .templateHash = contentHash(k_anchorHash),
                            .sourceHash   = contentHash(k_sourceHash),
                        },
                    },
                }
            );

            auto references = std::vector<PageReference>{};
            references.emplace_back(
                test::reference(pageId, anchorId, test::identifiesAs())
            );
            references.emplace_back(
                test::reference(pageId, actionId, test::interacts())
            );

            auto result = RuntimeManifest::create(
                test::projectId(std::move(project)),
                fingerprint,
                std::move(recognizers),
                {test::page(pageId, "home")},
                std::move(references)
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
        // Four tables, in one order. An element row carries only what is true of
        // the element everywhere -- its region and the set of things it can be
        // used for -- because a rectangle now serves several uses at once and
        // none of them owns the row. The pixels moved out to one variant row per
        // appearance, keyed back by element_id, so the rows stay one field per
        // line. And a page row carries only its identity: what a page requires
        // and forbids is derived from the reference rows below it, which are
        // also what authorises a click, so no fact is written twice.
        auto const expected = std::string{
            "schema = \"umbraflow-annotations/v2\"\n"
            "project_id = \"personal.test\"\n"
            "base_resolution = [8, 6]\n"
            "base_dpi = [96, 96]\n"
            "\n"
            "[[recognizer]]\n"
            "id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"home_marker\"\n"
            "search_roi = [0, 0, 3, 3]\n"
            "capabilities = [\"identify\"]\n"
            "\n"
            "[[recognizer]]\n"
            "id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"daily_button\"\n"
            "search_roi = [3, 2, 4, 3]\n"
            "capabilities = [\"interact\"]\n"
            "default_click = [1, 0]\n"
            "\n"
            "[[variant]]\n"
            "element_id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"only\"\n"
            "kind = \"gray_template\"\n"
            "template = \"assets/templates/1111111111111111111111111111111111111111111111111111111111111111.png\"\n"
            "template_hash = \"sha256:1111111111111111111111111111111111111111111111111111111111111111\"\n"
            "source_hash = \"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "template_rect = [1, 1, 1, 1]\n"
            "min_similarity_bp = 9000\n"
            "\n"
            "[[variant]]\n"
            "element_id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"only\"\n"
            "kind = \"gray_template\"\n"
            "template = \"assets/templates/2222222222222222222222222222222222222222222222222222222222222222.png\"\n"
            "template_hash = \"sha256:2222222222222222222222222222222222222222222222222222222222222222\"\n"
            "source_hash = \"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"\n"
            "template_rect = [4, 3, 2, 1]\n"
            "min_similarity_bp = 9000\n"
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
        };
        CHECK(encoded == expected);
        CHECK(encoded.find("source_id") == std::string::npos);
        CHECK(encoded.find("captured_at") == std::string::npos);
        CHECK(encoded.find("target_generation") == std::string::npos);
        // The colour key that produced a mask is authoring truth: the runtime
        // reads the mask off the compiled template's alpha channel instead.
        CHECK(encoded.find("colour_key") == std::string::npos);

        auto const parsed = parseRuntimeManifest(encoded);
        REQUIRE(parsed.has_value());
        CHECK(serializeRuntimeManifest(*parsed) == encoded);
        CHECK(parsed->catalog().recognizers().size() == 2U);
        CHECK(parsed->catalog().pages().size() == 1U);
        CHECK(parsed->catalog().references().size() == 2U);
        auto const* p_asset = parsed->findAsset(
            test::elementId(k_anchorId),
            test::resourceName("only")
        );
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
                "schema = \"umbraflow-annotations/v2\"\n"
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

    TEST_CASE("annotation runtime manifest reader refuses the schema it retired")
    {
        // Named separately from the drift table below, and asserted on the
        // message rather than only the kind. The canonical round-trip check at
        // the end of the parser would refuse these same bytes for its own
        // reason, so without the message this case would stay green with the
        // schema comparison deleted.
        auto const canonical = serializeRuntimeManifest(runtimeManifest());
        auto const retired   = replaceOnce(
            canonical,
            "umbraflow-annotations/v2",
            "umbraflow-annotations/v1"
        );
        auto const rejected = parseRuntimeManifest(retired);
        REQUIRE_FALSE(rejected.has_value());
        test::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(rejected.error().message().contains("unsupported"));
        CHECK(rejected.error().message().contains("schema"));
    }

    TEST_CASE("annotation runtime manifest reader rejects non-canonical or drifting input")
    {
        auto const canonical = serializeRuntimeManifest(runtimeManifest());
        auto invalid = std::vector<std::string>{};
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
            replaceOnce(canonical, "capabilities = [\"identify\"]", "capabilities = [\"guess\"]")
        );
        invalid.emplace_back(
            replaceOnce(canonical, "holding = \"owned\"", "holding = \"borrowed\"")
        );
        invalid.emplace_back(
            replaceOnce(
                canonical,
                "signature_role = \"required\"",
                "signature_role = \"maybe\""
            )
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
