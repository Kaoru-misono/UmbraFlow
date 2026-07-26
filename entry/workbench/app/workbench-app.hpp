#pragma once

#include "authoring-edit.hpp"
#include "preview.hpp"
#include "source-ingestion.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace uf::workbench
{
    // Mints a fresh identifier for a new authoring resource as an RFC 4122
    // version-4 UUID: 122 random bits with the version nibble stamped to 0100
    // and the variant bits to 10. A v4 UUID is the project's stable id
    // convention and its entropy makes an authoring-time collision negligible.
    // Only authoring mints ids, so a std::random_device fill is sufficient; the
    // runtime never mints and therefore imposes no determinism requirement here.
    [[nodiscard]]
    auto mintResourceId() -> annotation::ResourceId;

    // The canvas viewport as edited by the canvas panel. Zoom is a pixels-per-
    // source-pixel scale and the pan is the top-left source pixel mapped to the
    // canvas origin; both are plain view state that never feeds the document.
    struct CanvasView final
    {
        float m_zoom{1.0F};
        float m_panX{};
        float m_panY{};
    };

    // The complete, platform-free editing state behind the workbench window. It
    // owns the undo/redo history of the authoring document, a content-addressed
    // cache of the decoded source assets that back preview and display, the
    // current selection and canvas view, and the most recent preview outcome.
    // The document (through the history) is the single source of truth for which
    // sources exist; the asset cache is keyed by SourceId, is never rolled back
    // by undo/redo, and is filtered to the document whenever compiler inputs are
    // assembled. Every document mutation routes through applyEdit so the history
    // and dirty flag stay consistent; the GUI layer only reads and calls these
    // methods and never touches the history directly.
    class AppState final
    {
        std::filesystem::path m_projectRoot;
        AuthoringEditHistory  m_history;

        // A content-addressed cache of source assets keyed by SourceId. Undo and
        // redo never touch it, so an entry whose import was undone lingers as a
        // harmless orphan that lets redo restore the source without re-decoding;
        // orphans are pruned only once a save persists the document.
        std::vector<annotation::AuthoringSourceAsset> m_sources;

        std::optional<annotation::SourceId>     m_selectedSourceId{};
        std::optional<annotation::RecognizerId> m_selectedRecognizerId{};
        CanvasView                              m_canvasView{};
        std::optional<PreviewResult>            m_lastPreview{};
        std::optional<ModelCheck>               m_lastModelCheck{};
        bool                                    m_dirty{};

    public:
        AppState(
            std::filesystem::path projectRoot,
            annotation::AuthoringDocument document,
            std::vector<annotation::AuthoringSourceAsset> sources
        );

        // Builds a fresh session around an empty project rooted at projectRoot.
        // The seeded fingerprint is a placeholder until the first source is
        // ingested; construction only fails if the empty document is rejected.
        [[nodiscard]]
        static auto createEmpty(
            std::filesystem::path projectRoot
        ) -> Result<AppState>;

        [[nodiscard]]
        auto projectRoot() const noexcept UF_LIFETIME_BOUND
            -> std::filesystem::path const&;

        [[nodiscard]]
        auto document() const noexcept UF_LIFETIME_BOUND
            -> annotation::AuthoringDocument const&;

        [[nodiscard]] auto draft() const -> AuthoringDraft;
        [[nodiscard]] auto canUndo() const noexcept -> bool;
        [[nodiscard]] auto canRedo() const noexcept -> bool;

        // The whole asset cache, including orphans. Callers look an asset up by
        // SourceId, for which a lingering orphan is harmless; assembling compiler
        // inputs must instead go through compilerSourceAssets.
        [[nodiscard]]
        auto sources() const noexcept UF_LIFETIME_BOUND
            -> std::span<annotation::AuthoringSourceAsset const>;

        // The source assets for exactly the document's sources, in document
        // order, copied out of the cache so compilation and preview never see an
        // orphan or a stale ordering. Fails when a document source has no cached
        // asset: that is an internal invariant breach between the document and
        // the cache rather than a user error, and the failure names the id.
        [[nodiscard]]
        auto compilerSourceAssets() const
            -> Result<std::vector<annotation::AuthoringSourceAsset>>;

        [[nodiscard]]
        auto selectedSourceId() const noexcept
            -> std::optional<annotation::SourceId>;

        [[nodiscard]]
        auto selectedRecognizerId() const noexcept
            -> std::optional<annotation::RecognizerId>;

        [[nodiscard]] auto canvasView() const noexcept -> CanvasView;

        [[nodiscard]]
        auto lastPreview() const noexcept UF_LIFETIME_BOUND
            -> std::optional<PreviewResult> const&;

        // The most recent whole-model check, or nothing when none has been run
        // since the last edit. Like the preview it describes one revision of the
        // document, so any committed mutation clears it rather than leaving a
        // verdict that no longer refers to what is on screen.
        [[nodiscard]]
        auto lastModelCheck() const noexcept UF_LIFETIME_BOUND
            -> std::optional<ModelCheck> const&;

        [[nodiscard]] auto dirty() const noexcept -> bool;

        // Commits an edited draft through the history. Returns true when the
        // draft differed from the current document and became a new undo entry,
        // marking the state dirty; returns false for an identical draft and
        // propagates a build failure for an invalid one, leaving history intact.
        [[nodiscard]]
        auto applyEdit(AuthoringDraft const& draft) -> Result<bool>;

        // Adds an ingested source: appends its immutable record through the edit
        // history and retains its PNG asset for preview and canvas display. The
        // first source also adopts its geometry as the project fingerprint, since
        // createEmpty seeds only a placeholder. Selects the new source and reports
        // whether the document changed; propagates a build rejection, such as a
        // geometry that disagrees with the sources already present.
        [[nodiscard]]
        auto addIngestedSource(IngestedSource source) -> Result<bool>;

        auto undo() -> bool;
        auto redo() -> bool;

        auto setSelectedSourceId(
            std::optional<annotation::SourceId> id
        ) noexcept -> void;

        auto setSelectedRecognizerId(
            std::optional<annotation::RecognizerId> id
        ) noexcept -> void;

        auto setCanvasView(CanvasView view) noexcept -> void;
        auto setLastPreview(PreviewResult preview) -> void;
        auto setLastModelCheck(ModelCheck check) -> void;

        // Records that the current document has been persisted: clears dirty
        // without touching the undo/redo history and prunes the asset cache to
        // exactly the document's sources, discarding orphans a save made
        // unreachable.
        auto markSaved() -> void;

    private:
        // Drops cache entries whose SourceId is absent from the current
        // document, run once a save has persisted that document.
        auto pruneSourceCacheToDocument() -> void;

        // Clears the source or recognizer selection when undo or redo moved the
        // document to a revision that no longer contains the selected entity, so
        // later edits never reference a dangling id.
        auto reconcileSelectionToDocument() -> void;
    };
}
