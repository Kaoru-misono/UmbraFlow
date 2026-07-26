#include "test-helpers.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/content-hash.hpp>

#include <core/numeric/checked-arithmetic.hpp>
#include <core/safety/checked-access.hpp>
#include <core/types/integer.hpp>

#include <domain/error.hpp>

#include <image/png.hpp>

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::annotation
{
    namespace
    {
        constexpr auto k_sourceId       = "00000000-0000-0000-0000-000000000201";
        constexpr auto k_secondSourceId = "00000000-0000-0000-0000-000000000202";
        constexpr auto k_anchorId       = "00000000-0000-0000-0000-000000000001";
        constexpr auto k_actionId       = "00000000-0000-0000-0000-000000000002";
        constexpr auto k_pageId         = "00000000-0000-0000-0000-000000000101";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        struct CompilerFixture final
        {
            AuthoringDocument    document;
            AuthoringSourceAsset sourceAsset;
        };

        struct CompilationWorkFixture final
        {
            AuthoringDocument                 document;
            std::vector<AuthoringSourceAsset> sourceAssets{};
        };

        [[nodiscard]]
        auto encodedSource(
            uint32 width    = 3,
            uint32 height   = 2,
            uint8 redOffset = 0
        ) -> std::vector<std::byte>
        {
            auto const pixels = std::vector<std::byte>{
                asByte(1), asByte(2), asByte(3), asByte(255),
                asByte(4), asByte(5), asByte(6), asByte(255),
                asByte(7), asByte(8), asByte(9), asByte(255),
                asByte(10), asByte(11), asByte(12), asByte(255),
                asByte(13), asByte(14), asByte(15), asByte(255),
                asByte(16), asByte(17), asByte(18), asByte(255),
            };
            auto const area = checkedMultiply(
                static_cast<std::size_t>(width),
                static_cast<std::size_t>(height)
            );
            REQUIRE(area.has_value());
            auto const pixelCount = checkedMultiply(
                area.value_or(std::size_t{0}),
                std::size_t{4}
            );
            REQUIRE(pixelCount.has_value());
            auto resized = pixels;
            resized.resize(pixelCount.value_or(std::size_t{0}), asByte(0));
            for (auto index = std::size_t{0}; index < resized.size(); index += 4U)
            {
                checkedAt(resized, index) ^= asByte(redOffset);
            }
            auto encoded = image::encodeRgbaPng(
                "authoring-source.png",
                width,
                height,
                resized
            );
            REQUIRE(encoded.has_value());
            return *std::move(encoded);
        }

        [[nodiscard]]
        auto compilerFixture() -> CompilerFixture
        {
            auto const fingerprint = test::fingerprint(3, 2, 96, 96);
            auto const sourceId    = test::sourceId(k_sourceId);
            auto pngBytes          = encodedSource();
            auto const sourceHash  = sha256(pngBytes);
            REQUIRE(sourceHash.has_value());
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            auto const anchorId = test::recognizerId(k_anchorId);
            auto const actionId = test::recognizerId(k_actionId);
            auto const pageId   = test::pageId(k_pageId);
            auto const click    = TemplateOffset::create(1, 1, 2, 2);
            REQUIRE(click.has_value());
            auto elements = std::vector<Element>{};
            elements.emplace_back(
                test::anchorElement(
                    fingerprint,
                    anchorId,
                    "home_marker",
                    sourceId,
                    test::pixelRect(0, 0, 1, 1),
                    test::pixelRect(0, 0, 3, 2)
                )
            );
            elements.emplace_back(
                test::interactiveElement(
                    fingerprint,
                    actionId,
                    "daily_button",
                    sourceId,
                    test::pixelRect(1, 0, 2, 2),
                    test::pixelRect(0, 0, 3, 2),
                    *click
                )
            );
            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {*source},
                std::move(elements),
                {test::page(pageId, "home", {anchorId})},
                {test::placement(pageId, actionId, test::pixelRect(0, 0, 3, 2))},
                {}
            );
            REQUIRE(document.has_value());
            return CompilerFixture{
                .document    = *std::move(document),
                .sourceAsset = AuthoringSourceAsset{
                    .id       = sourceId,
                    .pngBytes = std::move(pngBytes),
                },
            };
        }

        [[nodiscard]]
        auto workloadFixture(
            std::span<PixelRect const> templateRects
        ) -> CompilerFixture
        {
            REQUIRE_FALSE(templateRects.empty());

            auto const fingerprint = test::fingerprint(8192, 8192, 96, 96);
            auto const sourceId    = test::sourceId(k_sourceId);
            auto const sourceBytes = std::vector<std::byte>{};
            auto const sourceHash  = sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .id          = sourceId,
                    .contentHash = *sourceHash,
                    .fingerprint = fingerprint,
                    .provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());

            auto elements      = std::vector<Element>{};
            auto recognizerIds = std::vector<RecognizerId>{};
            elements.reserve(templateRects.size());
            recognizerIds.reserve(templateRects.size());
            auto const searchRoi = test::pixelRect(0, 0, 8192, 8192);
            for (auto index = std::size_t{0}; index < templateRects.size(); ++index)
            {
                auto const recognizerId = test::recognizerId(
                    std::format(
                        "00000000-0000-0000-0000-{:012x}",
                        index + 0x300U
                    )
                );
                recognizerIds.emplace_back(recognizerId);
                elements.emplace_back(
                    test::anchorElement(
                        fingerprint,
                        recognizerId,
                        std::format("work_item_{}", index),
                        sourceId,
                        checkedAt(templateRects, index),
                        searchRoi
                    )
                );
            }

            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {*source},
                std::move(elements),
                {
                    test::page(
                        test::pageId(k_pageId),
                        "workload",
                        {recognizerIds.front()}
                    )
                },
                {},
                {}
            );
            REQUIRE(document.has_value());
            return CompilerFixture{
                .document    = *std::move(document),
                .sourceAsset = AuthoringSourceAsset{
                    .id       = sourceId,
                    .pngBytes = sourceBytes,
                },
            };
        }

        [[nodiscard]]
        auto compilationBoundaryFixture(
            std::span<PixelRect const> templateRects
        ) -> CompilationWorkFixture
        {
            auto const fingerprint = test::fingerprint(8192, 8192, 96, 96);
            auto const sourceBytes = std::vector<std::byte>{};
            auto const sourceHash  = sha256(sourceBytes);
            REQUIRE(sourceHash.has_value());

            auto sources      = std::vector<AuthoringSource>{};
            auto sourceIds    = std::vector<SourceId>{};
            auto sourceAssets = std::vector<AuthoringSourceAsset>{};
            sources.reserve(4);
            sourceIds.reserve(4);
            sourceAssets.reserve(4);
            for (auto index = std::size_t{0}; index < 4U; ++index)
            {
                auto const sourceId = test::sourceId(
                    std::format(
                        "00000000-0000-0000-0000-{:012x}",
                        index + 0x400U
                    )
                );
                auto source = AuthoringSource::create(
                    AuthoringSourceSpec{
                        .id          = sourceId,
                        .contentHash = *sourceHash,
                        .fingerprint = fingerprint,
                        .provenance  = ImportedSourceProvenance{},
                    }
                );
                REQUIRE(source.has_value());
                sources.emplace_back(*std::move(source));
                sourceIds.emplace_back(sourceId);
                sourceAssets.emplace_back(
                    AuthoringSourceAsset{
                        .id       = sourceId,
                        .pngBytes = sourceBytes,
                    }
                );
            }

            auto elements = std::vector<Element>{};
            elements.reserve(templateRects.size());
            auto const searchRoi = test::pixelRect(0, 0, 8192, 8192);
            for (auto index = std::size_t{0}; index < templateRects.size(); ++index)
            {
                auto const recognizerId = test::recognizerId(
                    std::format(
                        "00000000-0000-0000-0000-{:012x}",
                        index + 0x500U
                    )
                );
                elements.emplace_back(
                    test::infoElement(
                        fingerprint,
                        recognizerId,
                        std::format("boundary_item_{}", index),
                        sourceIds.front(),
                        checkedAt(templateRects, index),
                        searchRoi
                    )
                );
            }

            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                std::move(sources),
                std::move(elements),
                {},
                {},
                {}
            );
            REQUIRE(document.has_value());
            return CompilationWorkFixture{
                .document     = *std::move(document),
                .sourceAssets = std::move(sourceAssets),
            };
        }
    }

    TEST_CASE("annotation authoring compilation is deterministic and runtime-complete")
    {
        auto const fixture       = compilerFixture();
        auto const authoringToml = serializeAuthoringDocument(fixture.document);
        CHECK(authoringToml.find("capture_backend = \"imported\"") != std::string::npos);
        CHECK(authoringToml.find("target_generation") == std::string::npos);
        auto const reopened = parseAuthoringDocument(authoringToml);
        REQUIRE(reopened.has_value());

        auto const assets = std::span{&fixture.sourceAsset, std::size_t{1}};
        auto const first  = compileAuthoringDocument(fixture.document, assets);
        auto const second = compileAuthoringDocument(*reopened, assets);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->runtimeManifestToml == second->runtimeManifestToml);
        CHECK(first->templateAssets.size() == 2U);
        CHECK(second->templateAssets.size() == 2U);
        for (auto index = std::size_t{0}; index < first->templateAssets.size(); ++index)
        {
            CHECK(
                checkedAt(first->templateAssets, index).hash
                == checkedAt(second->templateAssets, index).hash
            );
            CHECK(
                checkedAt(first->templateAssets, index).pngBytes
                == checkedAt(second->templateAssets, index).pngBytes
            );
        }

        auto const parsed = parseRuntimeManifest(first->runtimeManifestToml);
        REQUIRE(parsed.has_value());
        CHECK(parsed->catalog().recognizers().size() == 2U);
        CHECK(parsed->catalog().pages().size() == 1U);
        for (auto const& asset : parsed->assets())
        {
            CHECK(asset.sourceHash == fixture.document.sources().front().contentHash());
        }
    }

    TEST_CASE("annotation authoring compilation preserves source relationships")
    {
        auto fixture   = compilerFixture();
        auto secondPng = encodedSource(3, 2, 0x40);
        auto const secondHash = sha256(secondPng);
        REQUIRE(secondHash.has_value());

        auto authoringToml = serializeAuthoringDocument(fixture.document);

        auto const annotationPosition = authoringToml.find("\n[[annotation]]");
        REQUIRE(annotationPosition != std::string::npos);
        authoringToml.insert(
            annotationPosition,
            std::format(
                "\n[[source]]\n"
                "id = \"{}\"\n"
                "path = \"assets/sources/{}.png\"\n"
                "content_hash = \"{}\"\n"
                "client_size = [3, 2]\n"
                "dpi = [96, 96]\n"
                "capture_backend = \"imported\"\n",
                k_secondSourceId,
                secondHash->hex(),
                secondHash->toString()
            )
        );
        auto const actionPosition = authoringToml.find("name = \"daily_button\"");
        REQUIRE(actionPosition != std::string::npos);
        auto const relationshipPosition = authoringToml.find(
            std::string{"source_id = \""} + k_sourceId + '"',
            actionPosition
        );
        REQUIRE(relationshipPosition != std::string::npos);
        authoringToml.replace(
            relationshipPosition,
            std::string_view{"source_id = \""}.size() + std::string_view{k_sourceId}.size() + 1U,
            std::string{"source_id = \""} + k_secondSourceId + '"'
        );

        auto const document = parseAuthoringDocument(authoringToml);
        REQUIRE(document.has_value());
        auto const firstHash = sha256(fixture.sourceAsset.pngBytes);
        REQUIRE(firstHash.has_value());
        auto const assets = std::array{
            AuthoringSourceAsset{
                .id       = test::sourceId(k_secondSourceId),
                .pngBytes = std::move(secondPng),
            },
            fixture.sourceAsset,
        };
        auto const compiled = compileAuthoringDocument(*document, assets);
        REQUIRE(compiled.has_value());

        auto const* p_anchor = compiled->runtimeManifest.findAsset(
            test::recognizerId(k_anchorId)
        );
        auto const* p_action = compiled->runtimeManifest.findAsset(
            test::recognizerId(k_actionId)
        );
        REQUIRE(p_anchor != nullptr);
        REQUIRE(p_action != nullptr);
        CHECK(p_anchor->sourceHash == *firstHash);
        CHECK(p_action->sourceHash == *secondHash);
        CHECK(p_anchor->templateHash != p_action->templateHash);
    }

    TEST_CASE("annotation authoring compilation deduplicates identical template crops")
    {
        auto const fixture = compilerFixture();
        auto authoringToml = serializeAuthoringDocument(fixture.document);

        auto const rectPosition = authoringToml.find("template_rect = [1, 0, 2, 2]");
        REQUIRE(rectPosition != std::string::npos);
        authoringToml.replace(
            rectPosition,
            std::string_view{"template_rect = [1, 0, 2, 2]"}.size(),
            "template_rect = [0, 0, 1, 1]"
        );
        auto const clickPosition = authoringToml.find("default_click = [1, 1]");
        REQUIRE(clickPosition != std::string::npos);
        authoringToml.replace(
            clickPosition,
            std::string_view{"default_click = [1, 1]"}.size(),
            "default_click = [0, 0]"
        );

        auto const document = parseAuthoringDocument(authoringToml);
        REQUIRE(document.has_value());
        auto const assets   = std::span{&fixture.sourceAsset, std::size_t{1}};
        auto const compiled = compileAuthoringDocument(*document, assets);
        REQUIRE(compiled.has_value());
        REQUIRE(compiled->runtimeManifest.assets().size() == 2U);
        CHECK(compiled->templateAssets.size() == 1U);
        CHECK(
            compiled->runtimeManifest.assets().front().templateHash
            == compiled->runtimeManifest.assets().back().templateHash
        );
    }

    TEST_CASE("annotation authoring compilation allows the exact pixel-work boundary")
    {
        auto const fixture = compilationBoundaryFixture(
            std::span<PixelRect const>{}
        );

        auto const result = compileAuthoringDocument(
            fixture.document,
            fixture.sourceAssets
        );
        REQUIRE_FALSE(result.has_value());
        test::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().contains("empty PNG"));
        CHECK_FALSE(result.error().message().contains("pixel work quota"));
    }

    TEST_CASE("annotation authoring compilation rejects one pixel above the work boundary")
    {
        auto const fixture = compilationBoundaryFixture(
            std::array{
                test::pixelRect(0, 0, 1, 1),
            }
        );

        auto const rejected = compileAuthoringDocument(
            fixture.document,
            fixture.sourceAssets
        );
        REQUIRE_FALSE(rejected.has_value());
        test::requireErrorKind(
            rejected.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(
            rejected.error().message()
            == "authoring compilation exceeds the 256 Mi-pixel work quota"
        );
    }

    TEST_CASE("annotation authoring compilation counts identical template tasks once")
    {
        auto const repeatedRect = test::pixelRect(0, 0, 8192, 8192);
        auto const fixture      = workloadFixture(
            std::array{
                repeatedRect,
                repeatedRect,
                repeatedRect,
                repeatedRect,
                repeatedRect,
            }
        );

        auto const result = compileAuthoringDocument(
            fixture.document,
            std::span{
                &fixture.sourceAsset,
                std::size_t{1}
            }
        );
        REQUIRE_FALSE(result.has_value());
        test::requireErrorKind(
            result.error(),
            AutomationErrorKind::InvalidResource
        );
        CHECK(result.error().message().contains("empty PNG"));
        CHECK_FALSE(
            result.error().message().contains("compilation work quota")
        );
    }

    TEST_CASE("annotation authoring compilation rejects missing or tampered source closure")
    {
        auto const fixture = compilerFixture();
        auto const missing = compileAuthoringDocument(fixture.document, {});
        REQUIRE_FALSE(missing.has_value());
        test::requireErrorKind(
            missing.error(),
            AutomationErrorKind::InvalidResource
        );

        auto tamperedAsset = fixture.sourceAsset;
        REQUIRE_FALSE(tamperedAsset.pngBytes.empty());
        tamperedAsset.pngBytes.front() ^= std::byte{0x01};
        auto const tampered = compileAuthoringDocument(
            fixture.document,
            std::span{&tamperedAsset, std::size_t{1}}
        );
        REQUIRE_FALSE(tampered.has_value());
        test::requireErrorKind(
            tampered.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("annotation authoring compilation rejects incompatible source geometry")
    {
        auto fixture = compilerFixture();

        fixture.sourceAsset.pngBytes = encodedSource(2, 2);

        auto const replacementHash = sha256(fixture.sourceAsset.pngBytes);
        REQUIRE(replacementHash.has_value());
        auto const fingerprint = test::fingerprint(3, 2, 96, 96);
        auto replacementSource = AuthoringSource::create(
            AuthoringSourceSpec{
                .id          = fixture.sourceAsset.id,
                .contentHash = *replacementHash,
                .fingerprint = fingerprint,
                .provenance  = ImportedSourceProvenance{},
            }
        );
        REQUIRE(replacementSource.has_value());
        auto const anchorId = test::recognizerId(k_anchorId);
        auto const pageId   = test::pageId(k_pageId);
        auto replacementDocument = AuthoringDocument::create(
            test::projectId(),
            fingerprint,
            {*replacementSource},
            {
                test::anchorElement(
                    fingerprint,
                    anchorId,
                    "home_marker",
                    fixture.sourceAsset.id,
                    test::pixelRect(0, 0, 1, 1),
                    test::pixelRect(0, 0, 3, 2)
                ),
            },
            {test::page(pageId, "home", {anchorId})},
            {},
            {}
        );
        REQUIRE(replacementDocument.has_value());

        auto const incompatible = compileAuthoringDocument(
            *replacementDocument,
            std::span{&fixture.sourceAsset, std::size_t{1}}
        );
        REQUIRE_FALSE(incompatible.has_value());
        test::requireErrorKind(
            incompatible.error(),
            AutomationErrorKind::InvalidResource
        );
    }

    TEST_CASE("annotation authoring compilation inverts a placement into the runtime manifest")
    {
        // Golden manifest. The placements-to-allowed_page_ids inversion is the
        // property that outlives the v1 authoring schema: a native v2 model with
        // one interactive element placed on one page must compile to the frozen
        // runtime manifest (umbraflow-annotations/v1) whose action target carries
        // that page inverted back onto the recognizer. The template hashes are
        // stable for the fixed source PNG, so the whole manifest is checked byte
        // for byte -- a drift in the inversion changes these bytes.
        auto const fixture  = compilerFixture();
        auto const assets   = std::span{&fixture.sourceAsset, std::size_t{1}};
        auto const compiled = compileAuthoringDocument(fixture.document, assets);
        REQUIRE(compiled.has_value());

        auto const expected = std::string{
            "schema = \"umbraflow-annotations/v1\"\n"
            "project_id = \"personal.test\"\n"
            "base_resolution = [3, 2]\n"
            "base_dpi = [96, 96]\n"
            "\n"
            "[[recognizer]]\n"
            "id = \"00000000-0000-0000-0000-000000000001\"\n"
            "name = \"home_marker\"\n"
            "annotation_type = \"page_anchor\"\n"
            "kind = \"gray_template\"\n"
            "template = \"assets/templates/"
            "552f203b92a92ab65529bd1e19a1b5e1dea3e5716d36332c80ab7a249eb7da45.png\"\n"
            "template_hash = \"sha256:"
            "552f203b92a92ab65529bd1e19a1b5e1dea3e5716d36332c80ab7a249eb7da45\"\n"
            "source_hash = \"sha256:"
            "cb6852a70d7af028e037d78d5bb0e55c7ea05a8c9054ce0b0f45fa8a31b0a2c2\"\n"
            "template_rect = [0, 0, 1, 1]\n"
            "search_roi = [0, 0, 3, 2]\n"
            "min_similarity_bp = 9000\n"
            "\n"
            "[[recognizer]]\n"
            "id = \"00000000-0000-0000-0000-000000000002\"\n"
            "name = \"daily_button\"\n"
            "annotation_type = \"action_target\"\n"
            "kind = \"gray_template\"\n"
            "template = \"assets/templates/"
            "65b3b7b8b17a5cb75f26ee782594839c22dac166f28b02ef171d59d43ab69d90.png\"\n"
            "template_hash = \"sha256:"
            "65b3b7b8b17a5cb75f26ee782594839c22dac166f28b02ef171d59d43ab69d90\"\n"
            "source_hash = \"sha256:"
            "cb6852a70d7af028e037d78d5bb0e55c7ea05a8c9054ce0b0f45fa8a31b0a2c2\"\n"
            "template_rect = [1, 0, 2, 2]\n"
            "search_roi = [0, 0, 3, 2]\n"
            "min_similarity_bp = 9000\n"
            "default_click = [1, 1]\n"
            "allowed_page_ids = [\"00000000-0000-0000-0000-000000000101\"]\n"
            "\n"
            "[[page]]\n"
            "id = \"00000000-0000-0000-0000-000000000101\"\n"
            "name = \"home\"\n"
            "required = [\"00000000-0000-0000-0000-000000000001\"]\n"
            "forbidden = []\n"
        };
        CHECK(compiled->runtimeManifestToml == expected);
    }

    TEST_CASE("annotation authoring compilation expands per-page placement ROIs")
    {
        // One interactive element placed on two pages with DIFFERENT search
        // rectangles. The v1 copy model gave each page its own recognizer and ROI;
        // the v2 single-element model must reproduce that at generation time --
        // one runtime recognizer per placement, each with its own ROI and its own
        // single allowed page -- or every page would ship the element's one shared
        // range and lose its detection range.
        constexpr auto k_anchorTwoId = "00000000-0000-0000-0000-000000000003";
        constexpr auto k_pageTwoId   = "00000000-0000-0000-0000-000000000102";

        auto const fingerprint = test::fingerprint(3, 2, 96, 96);
        auto const sourceId    = test::sourceId(k_sourceId);
        auto pngBytes          = encodedSource();
        auto const sourceHash  = sha256(pngBytes);
        REQUIRE(sourceHash.has_value());
        auto source = AuthoringSource::create(
            AuthoringSourceSpec{
                .id          = sourceId,
                .contentHash = *sourceHash,
                .fingerprint = fingerprint,
                .provenance  = ImportedSourceProvenance{},
            }
        );
        REQUIRE(source.has_value());

        auto const anchorOneId = test::recognizerId(k_anchorId);
        auto const anchorTwoId = test::recognizerId(k_anchorTwoId);
        auto const actionId    = test::recognizerId(k_actionId);
        auto const pageOneId   = test::pageId(k_pageId);
        auto const pageTwoId   = test::pageId(k_pageTwoId);
        auto const roiOne      = test::pixelRect(0, 0, 3, 2);
        auto const roiTwo      = test::pixelRect(1, 0, 2, 2);

        // A distinct template crop for the action, so the single shared template
        // asset the two per-page recognizers point at is visible in the count.
        auto elements = std::vector<Element>{};
        elements.emplace_back(
            test::anchorElement(
                fingerprint,
                anchorOneId,
                "home_marker",
                sourceId,
                test::pixelRect(0, 0, 1, 1),
                test::pixelRect(0, 0, 3, 2)
            )
        );
        elements.emplace_back(
            test::anchorElement(
                fingerprint,
                anchorTwoId,
                "second_marker",
                sourceId,
                test::pixelRect(0, 0, 1, 1),
                test::pixelRect(0, 0, 3, 2)
            )
        );
        elements.emplace_back(
            test::interactiveElement(
                fingerprint,
                actionId,
                "daily_button",
                sourceId,
                test::pixelRect(1, 0, 2, 2),
                test::pixelRect(0, 0, 3, 2)
            )
        );

        auto document = AuthoringDocument::create(
            test::projectId(),
            fingerprint,
            {*source},
            std::move(elements),
            {
                test::page(pageOneId, "home", {anchorOneId}),
                test::page(pageTwoId, "second", {anchorTwoId}),
            },
            {
                test::placement(pageOneId, actionId, roiOne),
                test::placement(pageTwoId, actionId, roiTwo),
            },
            {}
        );
        REQUIRE(document.has_value());

        auto const asset = AuthoringSourceAsset{
            .id       = sourceId,
            .pngBytes = std::move(pngBytes),
        };
        auto const assets = std::span{&asset, std::size_t{1}};
        auto const first  = compileAuthoringDocument(*document, assets);
        auto const second = compileAuthoringDocument(*document, assets);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        // Derived ids and names are stable across compiles of the same document.
        CHECK(first->runtimeManifestToml == second->runtimeManifestToml);

        auto const& recognizers = first->runtimeManifest.catalog().recognizers();
        // Two anchors plus one recognizer per placement of the shared element.
        CHECK(recognizers.size() == 4U);

        auto const actionOn = [&recognizers](PageId page)
            -> RecognizerDefinition const*
        {
            for (auto const& recognizer : recognizers)
            {
                if (
                    recognizer.annotationType() == AnnotationType::ActionTarget
                    && recognizer.allowedPageIds().size() == 1U
                    && recognizer.allowedPageIds().front() == page
                )
                {
                    return &recognizer;
                }
            }
            return nullptr;
        };
        auto const* p_one = actionOn(pageOneId);
        auto const* p_two = actionOn(pageTwoId);
        REQUIRE(p_one != nullptr);
        REQUIRE(p_two != nullptr);

        // Each page ships its own detection range against a single allowed page.
        CHECK(p_one->searchRoi() == roiOne);
        CHECK(p_two->searchRoi() == roiTwo);
        CHECK(p_one->id() != p_two->id());
        CHECK(p_one->name().value() != p_two->name().value());
        CHECK(p_one->name().value().starts_with("daily_button"));
        CHECK(p_two->name().value().starts_with("daily_button"));

        // The two per-page recognizers share one template asset: the crop dedupes
        // by content hash, so N placements of one element cost one template.
        CHECK(first->templateAssets.size() == 2U);
        auto const* p_oneAsset = first->runtimeManifest.findAsset(p_one->id());
        auto const* p_twoAsset = first->runtimeManifest.findAsset(p_two->id());
        REQUIRE(p_oneAsset != nullptr);
        REQUIRE(p_twoAsset != nullptr);
        CHECK(p_oneAsset->templateHash == p_twoAsset->templateHash);
    }
}
