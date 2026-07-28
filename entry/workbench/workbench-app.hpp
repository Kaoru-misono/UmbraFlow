#pragma once

#include "authoring-edit.hpp"
#include "preview.hpp"
#include "source-ingestion.hpp"

#include <annotation/authoring-compiler.hpp>
#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <optional>
#include <span>
#include <variant>
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
        float zoom{1.0F};
        float panX{};
        float panY{};
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
    public:
        // A single-item, typed selection: the workbench's one source of truth
        // for what the panels, canvas, and inspector act on, replacing the
        // independent (selected source, selected recognizer) pair. Exactly one
        // thing is selected, or nothing:
        //   Screen  -- a captured screen, with no element.
        //   Page    -- a page (no UI selects one yet; the U1b tree will).
        //   Element -- a recognizer, carrying the screen it is shown over so the
        //              canvas has an image to draw, and, when it was chosen from
        //              a page's member list, the page whose per-page search
        //              region the canvas edits in preference to the shown
        //              screen's claim.
        //
        // Invariants (U1a):
        //   * Selecting a Screen replaces the whole selection and so clears any
        //     element: a screen and an element are never both "selected".
        //   * An Element that names no shown screen inherits the currently shown
        //     one when installed through select(), so following a newly created
        //     entity leaves the shown image untouched exactly as the old paired
        //     setters did.
        //   * The stored preview is dropped whenever the shown screen changes and
        //     only then, so reselecting the same screen keeps a valid preview.
        class Selection final
        {
        public:
            struct Screen final
            {
                annotation::SourceId sourceId;
            };

            struct Page final
            {
                annotation::PageId pageId;
            };

            struct Element final
            {
                annotation::RecognizerId            recognizerId;
                std::optional<annotation::SourceId> shownScreen{};
                std::optional<annotation::PageId>   pageContext{};
            };

            Selection() noexcept = default;
            Selection(Screen screen) noexcept;
            Selection(Page page) noexcept;
            Selection(Element element) noexcept;

            // The three alternatives, each present only for its own kind.
            [[nodiscard]] auto asScreen() const noexcept -> std::optional<Screen>;
            [[nodiscard]] auto asPage() const noexcept -> std::optional<Page>;
            [[nodiscard]] auto asElement() const noexcept
                -> std::optional<Element>;

            // Derived reads shared by the migration-era accessors and the
            // canvas. shownScreen is the Screen itself or an Element's shown
            // screen, and a Page shows none; recognizer is an Element's id;
            // pageContext is an Element's page, the context the canvas prefers
            // over the shown screen's claim.
            [[nodiscard]] auto shownScreen() const noexcept
                -> std::optional<annotation::SourceId>;
            [[nodiscard]] auto recognizer() const noexcept
                -> std::optional<annotation::RecognizerId>;
            [[nodiscard]] auto pageContext() const noexcept
                -> std::optional<annotation::PageId>;

        private:
            std::variant<std::monostate, Screen, Page, Element> m_value{};
        };

    private:
        std::filesystem::path m_projectRoot;
        AuthoringEditHistory  m_history;

        // A content-addressed cache of source assets keyed by SourceId. Undo and
        // redo never touch it, so an entry whose import was undone lingers as a
        // harmless orphan that lets redo restore the source without re-decoding;
        // orphans are pruned only once a save persists the document.
        std::vector<annotation::AuthoringSourceAsset> m_sources;

        Selection                    m_selection{};
        CanvasView                   m_canvasView{};
        std::optional<PreviewResult> m_lastPreview{};
        std::optional<ModelCheck>    m_lastModelCheck{};

        // The edit-history position the document was last saved or loaded at.
        // dirty() is this differing from the history's current position, so an
        // undo back to a saved state reads clean and a redo past it reads dirty
        // again. A fresh history sits at position 0, which this default matches,
        // so a just-loaded project is clean.
        uint64 m_savedPosition{};

        // Whether a preview / model-check result that once existed was thrown away
        // by an edit, undo, redo, or shown-screen change without a fresh result
        // replacing it. The verify drawer reads these to tell "not run yet"
        // (result absent, flag clear) from "the project changed -- re-run" (result
        // absent, flag set). Cleared when a new result lands, so they never
        // outlive the staleness they describe.
        bool m_previewInvalidated{};
        bool m_modelCheckInvalidated{};

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

        // The edit history's current position, stamped onto an EditPage at
        // open() so a commit built against a version the author has since left
        // (through undo or redo) is refused rather than silently resurrecting
        // it. Advances on every committed change, undo, and redo.
        [[nodiscard]] auto revision() const noexcept -> uint64;

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

        // The whole typed selection, the source of truth the panels and canvas
        // read; selectedSourceId and selectedRecognizerId below are thin derived
        // views of it kept for the low-risk read sites.
        [[nodiscard]]
        auto selection() const noexcept UF_LIFETIME_BOUND -> Selection const&;

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

        // Whether a preview / model check that had produced a result has since
        // been invalidated with none run in its place. False both before the first
        // result and immediately after a fresh one, so the drawer's three states
        // (results / stale / empty) are decided by pairing this with lastPreview /
        // lastModelCheck rather than by a silent blank.
        [[nodiscard]] auto previewInvalidated() const noexcept -> bool;
        [[nodiscard]] auto modelCheckInvalidated() const noexcept -> bool;

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

        // Installs a whole typed selection, the single write path replacing the
        // old paired setters. An Element that names no shown screen inherits the
        // currently shown one, and the stored preview is dropped only when the
        // shown screen actually changes, so reselecting the same screen keeps it.
        auto select(Selection selection) noexcept -> void;

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

        // Discards the stored preview / model check and records that it was
        // invalidated, but only when one actually existed -- so the "stale" flag
        // never fires for a result that was never produced. setLastPreview /
        // setLastModelCheck clear the flag as they store a fresh result.
        auto invalidatePreview() -> void;
        auto invalidateModelCheck() -> void;

        // Degrades the typed selection when undo or redo moved the document to a
        // revision that no longer holds what it names, so later edits never
        // reference a dangling id: a deleted element falls back to the screen it
        // was shown over (or to nothing when that screen is gone too), a deleted
        // screen or page falls to nothing, and a surviving element drops a shown
        // screen or page context that vanished.
        auto reconcileSelectionToDocument() -> void;
    };
}
