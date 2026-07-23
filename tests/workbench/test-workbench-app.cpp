#include "../annotation/test-helpers.hpp"

#include <app/workbench-app.hpp>

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/content-hash.hpp>

#include <doctest/doctest.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        constexpr auto g_sourceId = "00000000-0000-0000-0000-000000000201";
        constexpr auto g_anchorId = "00000000-0000-0000-0000-000000000001";
        constexpr auto g_pageId   = "00000000-0000-0000-0000-000000000101";
        constexpr auto g_importA  = "00000000-0000-0000-0000-0000000002a1";
        constexpr auto g_importB  = "00000000-0000-0000-0000-0000000002b2";

        [[nodiscard]]
        auto document() -> annotation::AuthoringDocument
        {
            auto const fingerprint = annotation::test::fingerprint(8, 8, 96, 96);
            auto const sourceId    = annotation::test::sourceId(g_sourceId);
            auto const anchorId    = annotation::test::recognizerId(g_anchorId);
            auto const pageId      = annotation::test::pageId(g_pageId);
            auto const sourceHash  = annotation::sha256(
                std::span<std::byte const>{}
            );
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

            auto created = annotation::AuthoringDocument::create(
                annotation::test::projectId(),
                fingerprint,
                {*source},
                {
                    annotation::AuthoringRecognizerSpec{
                        .m_definition = annotation::test::recognizer(
                            fingerprint,
                            anchorId,
                            "home_marker",
                            annotation::AnnotationType::PageAnchor,
                            annotation::test::pixelRect(0, 0, 2, 2),
                            annotation::test::pixelRect(0, 0, 4, 4)
                        ),
                        .m_sourceId = sourceId,
                    },
                },
                {annotation::test::page(pageId, "home", {anchorId})},
                {}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        [[nodiscard]]
        auto appState() -> AppState
        {
            return AppState{std::filesystem::path{"personal.workbench"}, document(), {}};
        }

        [[nodiscard]]
        auto emptyState() -> AppState
        {
            auto created = AppState::createEmpty(
                std::filesystem::path{"personal.workbench"}
            );
            REQUIRE(created.has_value());
            return *std::move(created);
        }

        // A minimal ingestable source: the asset bytes are opaque here because no
        // test in this file compiles or decodes them, so a single marker byte and
        // its hash are enough to exercise the cache and the document record.
        [[nodiscard]]
        auto ingestedSource(
            std::string_view idText,
            annotation::ProjectFingerprint fingerprint,
            std::byte marker
        ) -> IngestedSource
        {
            auto const id   = annotation::test::sourceId(idText);
            auto bytes      = std::vector<std::byte>{marker};
            auto const hash = annotation::sha256(bytes);
            REQUIRE(hash.has_value());

            return IngestedSource{
                .m_spec = annotation::AuthoringSourceSpec{
                    .m_id          = id,
                    .m_contentHash = *hash,
                    .m_fingerprint = fingerprint,
                    .m_provenance  = annotation::ImportedSourceProvenance{},
                },
                .m_asset = annotation::AuthoringSourceAsset{
                    .m_id       = id,
                    .m_pngBytes = std::move(bytes),
                },
            };
        }
    }

    TEST_CASE("minted resource ids are well-formed version-4 UUIDs")
    {
        auto const id   = mintResourceId();
        auto const text = id.toString();

        REQUIRE(text.size() == 36U);
        CHECK(text.at(8) == '-');
        CHECK(text.at(13) == '-');
        CHECK(text.at(18) == '-');
        CHECK(text.at(23) == '-');

        // RFC 4122: the version nibble is 4 and the variant nibble is 8..b.
        CHECK(text.at(14) == '4');
        auto const variant = text.at(19);
        CHECK(
            (
                variant == '8'
                || variant == '9'
                || variant == 'a'
                || variant == 'b'
            )
        );

        auto const reparsed = annotation::ResourceId::parse(text);
        REQUIRE(reparsed.has_value());
        CHECK(*reparsed == id);
    }

    TEST_CASE("distinct mints do not collide")
    {
        CHECK(mintResourceId() != mintResourceId());
    }

    TEST_CASE("an empty project starts clean with no history")
    {
        auto const state = AppState::createEmpty(
            std::filesystem::path{"personal.workbench"}
        );
        REQUIRE(state.has_value());
        CHECK_FALSE(state->dirty());
        CHECK_FALSE(state->canUndo());
        CHECK_FALSE(state->canRedo());
        CHECK(state->document().catalog().recognizers().empty());
    }

    TEST_CASE("committed edits set the dirty flag and grow the undo history")
    {
        auto state = appState();
        CHECK_FALSE(state.dirty());
        CHECK_FALSE(state.canUndo());

        auto edited = state.draft();
        edited.m_recognizers.at(0).m_name = "renamed_marker";

        auto const applied = state.applyEdit(edited);
        REQUIRE(applied.has_value());
        CHECK(*applied);
        CHECK(state.dirty());
        CHECK(state.canUndo());
        CHECK_FALSE(state.canRedo());
        CHECK(state.draft().m_recognizers.at(0).m_name == "renamed_marker");

        state.markSaved();
        CHECK_FALSE(state.dirty());
    }

    TEST_CASE("an identical edit neither dirties the state nor records history")
    {
        auto state = appState();

        auto const applied = state.applyEdit(state.draft());
        REQUIRE(applied.has_value());
        CHECK_FALSE(*applied);
        CHECK_FALSE(state.dirty());
        CHECK_FALSE(state.canUndo());
    }

    TEST_CASE("undo and redo walk the committed document versions")
    {
        auto state = appState();

        auto edited = state.draft();
        edited.m_recognizers.at(0).m_name = "renamed_marker";
        REQUIRE(state.applyEdit(edited).has_value());

        CHECK(state.undo());
        CHECK(state.draft().m_recognizers.at(0).m_name == "home_marker");
        CHECK_FALSE(state.canUndo());
        CHECK(state.canRedo());

        CHECK(state.redo());
        CHECK(state.draft().m_recognizers.at(0).m_name == "renamed_marker");
        CHECK(state.canUndo());
        CHECK_FALSE(state.canRedo());
    }

    TEST_CASE("selection and canvas view round-trip through the state")
    {
        auto state = appState();

        auto const sourceId = annotation::test::sourceId(g_sourceId);
        state.setSelectedSourceId(sourceId);
        REQUIRE(state.selectedSourceId().has_value());
        CHECK(*state.selectedSourceId() == sourceId);

        state.setCanvasView(CanvasView{.m_zoom = 2.5F, .m_panX = 4.0F, .m_panY = 8.0F});
        CHECK(state.canvasView().m_zoom == doctest::Approx(2.5F));
        CHECK(state.canvasView().m_panX == doctest::Approx(4.0F));
        CHECK(state.canvasView().m_panY == doctest::Approx(8.0F));
    }

    TEST_CASE("compiler inputs after an undone import cover only document sources")
    {
        auto state          = emptyState();
        auto const finger   = annotation::test::fingerprint(4, 4, 96, 96);
        auto const imported = state.addIngestedSource(
            ingestedSource(g_importA, finger, std::byte{0x11})
        );
        REQUIRE(imported.has_value());
        CHECK(*imported);

        CHECK(state.undo());
        CHECK(state.document().sources().empty());

        // The undone import lingers in the cache, but the assembly the save path
        // uses filters it out, so it succeeds with no sources.
        auto const assets = state.compilerSourceAssets();
        REQUIRE(assets.has_value());
        CHECK(assets->empty());
    }

    TEST_CASE("redo restores an undone import for the compiler inputs")
    {
        auto state        = emptyState();
        auto const finger = annotation::test::fingerprint(4, 4, 96, 96);
        REQUIRE(
            state.addIngestedSource(
                ingestedSource(g_importA, finger, std::byte{0x11})
            ).has_value()
        );

        CHECK(state.undo());
        CHECK(state.redo());
        REQUIRE(state.document().sources().size() == 1U);

        auto const assets = state.compilerSourceAssets();
        REQUIRE(assets.has_value());
        REQUIRE(assets->size() == 1U);
        CHECK(assets->front().m_id == annotation::test::sourceId(g_importA));
    }

    TEST_CASE("importing after an undone import compiles exactly the newer source")
    {
        auto state        = emptyState();
        auto const finger = annotation::test::fingerprint(4, 4, 96, 96);
        REQUIRE(
            state.addIngestedSource(
                ingestedSource(g_importA, finger, std::byte{0x11})
            ).has_value()
        );
        CHECK(state.undo());
        REQUIRE(
            state.addIngestedSource(
                ingestedSource(g_importB, finger, std::byte{0x22})
            ).has_value()
        );

        auto const assets = state.compilerSourceAssets();
        REQUIRE(assets.has_value());
        REQUIRE(assets->size() == 1U);
        CHECK(assets->front().m_id == annotation::test::sourceId(g_importB));
    }

    TEST_CASE("a committed edit clears the stored preview")
    {
        auto state = appState();
        state.setLastPreview(PreviewResult{});
        REQUIRE(state.lastPreview().has_value());

        auto edited = state.draft();
        edited.m_recognizers.at(0).m_name = "renamed_marker";
        REQUIRE(state.applyEdit(edited).has_value());

        CHECK_FALSE(state.lastPreview().has_value());
    }
}
