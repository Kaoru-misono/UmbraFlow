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
        constexpr auto g_sourceId       = "00000000-0000-0000-0000-000000000201";
        constexpr auto g_secondSourceId = "00000000-0000-0000-0000-000000000202";
        constexpr auto g_anchorId       = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_actionId       = "00000000-0000-0000-0000-000000000002";
        constexpr auto g_pageId         = "00000000-0000-0000-0000-000000000101";

        [[nodiscard]]
        constexpr auto asByte(uint8 value) noexcept -> std::byte
        {
            return static_cast<std::byte>(value);
        }

        struct CompilerFixture final
        {
            AuthoringDocument    m_document;
            AuthoringSourceAsset m_sourceAsset;
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
            auto const pixelCount = checkedMultiply(*area, std::size_t{4});
            REQUIRE(pixelCount.has_value());
            auto resized = pixels;
            resized.resize(*pixelCount, asByte(0));
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
            auto const sourceId    = test::sourceId(g_sourceId);
            auto pngBytes          = encodedSource();
            auto const sourceHash  = sha256(pngBytes);
            REQUIRE(sourceHash.has_value());
            auto source = AuthoringSource::create(
                AuthoringSourceSpec{
                    .m_id          = sourceId,
                    .m_contentHash = *sourceHash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = ImportedSourceProvenance{},
                }
            );
            REQUIRE(source.has_value());
            auto const anchorId = test::recognizerId(g_anchorId);
            auto const actionId = test::recognizerId(g_actionId);
            auto const pageId   = test::pageId(g_pageId);
            auto const click    = TemplateOffset::create(1, 1, 2, 2);
            REQUIRE(click.has_value());
            auto document = AuthoringDocument::create(
                test::projectId(),
                fingerprint,
                {*source},
                {
                    AuthoringRecognizerSpec{
                        .m_definition = test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            AnnotationType::PageAnchor,
                            test::pixelRect(0, 0, 1, 1),
                            test::pixelRect(0, 0, 3, 2)
                        ),
                        .m_sourceId = sourceId,
                    },
                    AuthoringRecognizerSpec{
                        .m_definition = test::recognizer(
                            fingerprint,
                            actionId,
                            "daily_button",
                            AnnotationType::ActionTarget,
                            test::pixelRect(1, 0, 2, 2),
                            test::pixelRect(0, 0, 3, 2),
                            {pageId},
                            *click
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {test::page(pageId, "home", {anchorId})},
                {}
            );
            REQUIRE(document.has_value());
            return CompilerFixture{
                .m_document    = *std::move(document),
                .m_sourceAsset = AuthoringSourceAsset{
                    .m_id       = sourceId,
                    .m_pngBytes = std::move(pngBytes),
                },
            };
        }
    }

    TEST_CASE("annotation authoring compilation is deterministic and runtime-complete")
    {
        auto const fixture       = compilerFixture();
        auto const authoringToml = serializeAuthoringDocument(fixture.m_document);
        CHECK(authoringToml.find("capture_backend = \"imported\"") != std::string::npos);
        CHECK(authoringToml.find("target_generation") == std::string::npos);
        auto const reopened = parseAuthoringDocument(authoringToml);
        REQUIRE(reopened.has_value());

        auto const assets = std::span{&fixture.m_sourceAsset, std::size_t{1}};
        auto const first  = compileAuthoringDocument(fixture.m_document, assets);
        auto const second = compileAuthoringDocument(*reopened, assets);
        REQUIRE(first.has_value());
        REQUIRE(second.has_value());
        CHECK(first->m_runtimeManifestToml == second->m_runtimeManifestToml);
        CHECK(first->m_templateAssets.size() == 2U);
        CHECK(second->m_templateAssets.size() == 2U);
        for (auto index = std::size_t{0}; index < first->m_templateAssets.size(); ++index)
        {
            CHECK(
                checkedAt(first->m_templateAssets, index).m_hash
                == checkedAt(second->m_templateAssets, index).m_hash
            );
            CHECK(
                checkedAt(first->m_templateAssets, index).m_pngBytes
                == checkedAt(second->m_templateAssets, index).m_pngBytes
            );
        }

        auto const parsed = parseRuntimeManifest(first->m_runtimeManifestToml);
        REQUIRE(parsed.has_value());
        CHECK(parsed->catalog().recognizers().size() == 2U);
        CHECK(parsed->catalog().pages().size() == 1U);
        for (auto const& asset : parsed->assets())
        {
            CHECK(asset.m_sourceHash == fixture.m_document.sources().front().contentHash());
        }
    }

    TEST_CASE("annotation authoring compilation preserves source relationships")
    {
        auto fixture          = compilerFixture();
        auto secondPng        = encodedSource(3, 2, 0x40);
        auto const secondHash = sha256(secondPng);
        REQUIRE(secondHash.has_value());

        auto authoringToml = serializeAuthoringDocument(fixture.m_document);

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
                g_secondSourceId,
                secondHash->hex(),
                secondHash->toString()
            )
        );
        auto const actionPosition = authoringToml.find("name = \"daily_button\"");
        REQUIRE(actionPosition != std::string::npos);
        auto const relationshipPosition = authoringToml.find(
            std::string{"source_id = \""} + g_sourceId + '"',
            actionPosition
        );
        REQUIRE(relationshipPosition != std::string::npos);
        authoringToml.replace(
            relationshipPosition,
            std::string_view{"source_id = \""}.size() + std::string_view{g_sourceId}.size() + 1U,
            std::string{"source_id = \""} + g_secondSourceId + '"'
        );

        auto const document = parseAuthoringDocument(authoringToml);
        REQUIRE(document.has_value());
        auto const firstHash = sha256(fixture.m_sourceAsset.m_pngBytes);
        REQUIRE(firstHash.has_value());
        auto const assets = std::array{
            AuthoringSourceAsset{
                .m_id       = test::sourceId(g_secondSourceId),
                .m_pngBytes = std::move(secondPng),
            },
            fixture.m_sourceAsset,
        };
        auto const compiled = compileAuthoringDocument(*document, assets);
        REQUIRE(compiled.has_value());

        auto const* p_anchor = compiled->m_runtimeManifest.findAsset(
            test::recognizerId(g_anchorId)
        );
        auto const* p_action = compiled->m_runtimeManifest.findAsset(
            test::recognizerId(g_actionId)
        );
        REQUIRE(p_anchor != nullptr);
        REQUIRE(p_action != nullptr);
        CHECK(p_anchor->m_sourceHash == *firstHash);
        CHECK(p_action->m_sourceHash == *secondHash);
        CHECK(p_anchor->m_templateHash != p_action->m_templateHash);
    }

    TEST_CASE("annotation authoring compilation deduplicates identical template crops")
    {
        auto const fixture = compilerFixture();
        auto authoringToml = serializeAuthoringDocument(fixture.m_document);

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
        auto const assets   = std::span{&fixture.m_sourceAsset, std::size_t{1}};
        auto const compiled = compileAuthoringDocument(*document, assets);
        REQUIRE(compiled.has_value());
        REQUIRE(compiled->m_runtimeManifest.assets().size() == 2U);
        CHECK(compiled->m_templateAssets.size() == 1U);
        CHECK(
            compiled->m_runtimeManifest.assets().front().m_templateHash
            == compiled->m_runtimeManifest.assets().back().m_templateHash
        );
    }

    TEST_CASE("annotation authoring compilation rejects missing or tampered source closure")
    {
        auto const fixture = compilerFixture();
        auto const missing = compileAuthoringDocument(fixture.m_document, {});
        REQUIRE_FALSE(missing.has_value());
        test::requireErrorKind(
            missing.error(),
            AutomationErrorKind::InvalidResource
        );

        auto tamperedAsset = fixture.m_sourceAsset;
        REQUIRE_FALSE(tamperedAsset.m_pngBytes.empty());
        tamperedAsset.m_pngBytes.front() ^= std::byte{0x01};
        auto const tampered = compileAuthoringDocument(
            fixture.m_document,
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

        fixture.m_sourceAsset.m_pngBytes = encodedSource(2, 2);

        auto const replacementHash = sha256(fixture.m_sourceAsset.m_pngBytes);
        REQUIRE(replacementHash.has_value());
        auto const fingerprint = test::fingerprint(3, 2, 96, 96);
        auto replacementSource = AuthoringSource::create(
            AuthoringSourceSpec{
                .m_id          = fixture.m_sourceAsset.m_id,
                .m_contentHash = *replacementHash,
                .m_fingerprint = fingerprint,
                .m_provenance  = ImportedSourceProvenance{},
            }
        );
        REQUIRE(replacementSource.has_value());
        auto const anchorId = test::recognizerId(g_anchorId);
        auto const pageId   = test::pageId(g_pageId);
        auto replacementDocument = AuthoringDocument::create(
            test::projectId(),
            fingerprint,
            {*replacementSource},
            {
                AuthoringRecognizerSpec{
                    .m_definition = test::recognizer(
                        fingerprint,
                        anchorId,
                        "home_marker",
                        AnnotationType::PageAnchor,
                        test::pixelRect(0, 0, 1, 1),
                        test::pixelRect(0, 0, 3, 2)
                    ),
                    .m_sourceId = fixture.m_sourceAsset.m_id,
                },
            },
            {test::page(pageId, "home", {anchorId})},
            {}
        );
        REQUIRE(replacementDocument.has_value());

        auto const incompatible = compileAuthoringDocument(
            *replacementDocument,
            std::span{&fixture.m_sourceAsset, std::size_t{1}}
        );
        REQUIRE_FALSE(incompatible.has_value());
        test::requireErrorKind(
            incompatible.error(),
            AutomationErrorKind::InvalidResource
        );
    }
}
