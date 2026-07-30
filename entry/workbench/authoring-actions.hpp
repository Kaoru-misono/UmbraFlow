#pragma once

#include "authoring-edit.hpp"
#include "panel-state.hpp"
#include "workbench-app.hpp"

#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace uf::workbench
{
    // What a panel asks the authoring backend to do, and what the author is told
    // about the answer. None of it draws: a panel decides that a button was
    // pressed, and everything between that and the next document version happens
    // here, so it is reachable from test-workbench without a GUI.
    //
    // The rule these all obey is that no edit is committed while a panel is still
    // borrowing into the document. A request parks a complete draft on
    // PanelUiState and applyPendingEdit commits it once per frame, after the
    // panels that read the document have finished with it.

    // The first eight characters of an id, for a label that has to fit next to a
    // name without becoming the widest column in the panel.
    [[nodiscard]]
    auto shortId(annotation::ResourceId const& id) -> std::string;

    // A page's authored name, or a short form of its id when the document does
    // not hold that page. Never empty, because it goes straight into a label.
    [[nodiscard]]
    auto pageName(AppState const& state, annotation::PageId id) -> std::string;

    // The draft's copy of a recognizer, or nullptr when the draft does not hold
    // it. The pointer is valid only until the draft is rebuilt or committed.
    [[nodiscard]]
    auto findEditableRecognizer(
        AuthoringDraft& draft UF_LIFETIME_BOUND,
        annotation::ElementId id
    ) -> EditableRecognizer*;

    // The percentage a basis-point threshold is shown as, and the basis points a
    // shown percentage commits to. Persistence stays integer basis points (design
    // lock OQ-1 / §1.4): the UI shows a familiar percent to two decimals while the
    // document never sees a float. The pair round-trips -- taking an unedited
    // value from basis points to a percent and back is the identity -- and the
    // commit direction rounds to the nearest basis point and clamps to [0, 10000].
    [[nodiscard]]
    auto thresholdPercentFromBasisPoints(uint32 basisPoints) noexcept -> float;

    [[nodiscard]]
    auto thresholdBasisPointsFromPercent(float percent) noexcept -> uint32;

    // Duplicates the selected element as a new one and selects the copy once the
    // edit lands: a fresh id, a unique name derived from the original, and the
    // same region, capabilities, appearances, and page references. Reports a
    // refusal on the status line rather than committing a rejected edit.
    auto requestDuplicateElement(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId id
    ) -> void;

    // Parks a validated draft for this frame's single commit.
    auto requestEdit(
        PanelUiState& ui,
        AuthoringDraft draft,
        std::string description
    ) -> void;

    // As requestEdit, and selects the entity once the edit lands. Selecting
    // before it lands would leave the selection pointing at an id a rejected
    // edit never added. The severity is the one the description is reported at
    // once the edit commits, defaulting to Info for an ordinary success.
    auto requestEditSelecting(
        PanelUiState& ui,
        AuthoringDraft draft,
        std::string description,
        annotation::ElementId recognizerId,
        std::optional<annotation::SourceId> sourceId,
        LogSeverity severity = LogSeverity::Info
    ) -> void;

    // Commits this frame's parked edit, if any. Called once per frame, after the
    // panels that borrow into the document and before the one that mutates it.
    auto applyPendingEdit(AppState& state, PanelUiState& ui) -> void;

    // Queues a toolbar command for after this frame's parked edit is committed.
    // The toolbar draws at the top of the frame but must not run a save or an
    // undo inline: an undo has to see the same-frame widget-deactivation edit
    // already landed. One command per frame; a second press is dropped, matching
    // requestEdit's one-commit-per-frame rule.
    auto requestToolbarCommand(PanelUiState& ui, ToolbarCommand command) -> void;

    // Runs the queued toolbar command, if any, and clears it. Called once per
    // frame, immediately after applyPendingEdit, so save / undo / redo act on the
    // document the frame's edit already produced. Save and generate reports its
    // own outcome and marks the state saved; undo and redo report the resulting
    // catalog counts, exactly as the former Actions buttons did.
    auto dispatchToolbarCommand(AppState& state, PanelUiState& ui) -> void;

    // States what a deletion withdrew along the way, because a membership the
    // author did not ask to remove must not be silent.
    [[nodiscard]]
    auto deletionSummary(
        std::string_view what,
        DeletedEntity const& deleted
    ) -> std::string;

    // Parks a deletion the backend already validated, or reports why it refused.
    auto requestDeletion(
        PanelUiState& ui,
        Result<DeletedEntity> deleted,
        std::string_view what
    ) -> void;

    // Records a pageless regression expectation for one screen: that it must
    // resolve to none of the project's pages, or that it is allowed to be
    // ambiguous. A screen resolving to one page is a page-scoped act and is
    // recorded through EditPage from the Pages panel; these two name no page, so
    // they enter here. Parks the edit for this frame's commit, or reports why the
    // backend refused.
    auto requestScreenExpectation(
        AppState& state,
        PanelUiState& ui,
        annotation::SourceId source,
        PagelessExpectation expectation
    ) -> void;

    // The screen one element's pixels were cut from: the source of its sole
    // appearance. An element located by its page has no appearance and so no
    // screen of its own.
    [[nodiscard]]
    auto sourceOfRecognizer(
        AppState const& state,
        annotation::ElementId id
    ) -> std::optional<annotation::SourceId>;

    // The screen a page's regression case claims, if it has one.
    [[nodiscard]]
    auto claimedScreen(
        AppState const& state,
        annotation::PageId pageId
    ) -> std::optional<annotation::SourceId>;

    // The page whose members the canvas draws and hit-tests over a shown screen:
    // the page an element was selected under wins outright, and only without one
    // does it fall back to the page whose regression case resolves to the shown
    // screen. Nothing when the screen resolves to no page and no selection page is
    // supplied -- the canvas then has no page to draw, and draw-to-create is inert.
    [[nodiscard]]
    auto shownPageForScreen(
        AppState const& state,
        annotation::SourceId shownScreen,
        std::optional<annotation::PageId> selectionPage = std::nullopt
    ) -> std::optional<annotation::PageId>;

    // The page whose reference to an element the canvas edits when that element
    // is shown over a given screen, together with the region searched for it
    // there. Narrowed to the case the canvas can act on: the page references the
    // element, does not identify by it (a reference exercising identify reads the
    // element's own region and may not refine it), and the element is one that
    // can be clicked, which is the only handle the canvas has to write a per-page
    // region through. Empty otherwise, in which case the canvas falls back to the
    // element's own default range.
    //
    // The page is resolved by precedence: selectionPage -- the page an element
    // was selected under, from a page's member list -- wins outright; only
    // without one does the context fall back to the page that claims the shown
    // screen (the inverse of claimedScreen). This is what lets range editing on
    // an element selected under a page use that page even when the shown
    // screen's claim is missing or ambiguous.
    struct ReferenceContext final
    {
        annotation::PageId page;
        PixelRect          searchRoi;
    };

    [[nodiscard]]
    auto referenceContext(
        AppState const& state,
        annotation::ElementId id,
        annotation::SourceId shownScreen,
        std::optional<annotation::PageId> selectionPage = std::nullopt
    ) -> std::optional<ReferenceContext>;

    // Selects a recognizer and follows to the screen it is meaningful on:
    // preferredScreen when the caller knows it (a member picked from a page's
    // group), otherwise the screen its appearance was cut from. pageContext is
    // the page the element was selected under, if any, which the canvas then
    // prefers when resolving the reference it edits. Without a resolvable screen
    // the element inherits the currently shown one so its rectangles are not
    // drawn over whatever image the canvas happens to hold.
    auto selectRecognizer(
        AppState& state,
        annotation::ElementId id,
        std::optional<annotation::SourceId> preferredScreen,
        std::optional<annotation::PageId> pageContext = std::nullopt
    ) -> void;

    // The colour key one element carries, if any: the key on its sole
    // appearance. It is authoring-only -- the runtime reads the mask off the
    // compiled template's alpha and never sees a key -- so it is read off the
    // draft rather than off the catalog.
    [[nodiscard]]
    auto elementColourKey(
        AppState const& state,
        annotation::ElementId id
    ) -> std::optional<annotation::ColourKey>;

    // Sets or clears one element's colour key, reporting a refusal rather than
    // letting the picker snap back unexplained. One call is one undo entry, so
    // the picker commits on the end of a gesture rather than on every frame of
    // a slider drag.
    auto requestElementColourKey(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId id,
        std::optional<annotation::ColourKey> colourKey
    ) -> void;

    // Puts an element on another page as a borrowed reference and says at once
    // whether it is actually there.
    //
    // The reference starts with the region the element already searches, not the
    // whole frame: the same element usually sits in the same place, so that is
    // both the better first guess and a search small enough to score on the spot.
    // The score is measured against the page being dropped onto, and it is the
    // answer to the question the reference cannot answer on its own -- whether
    // these pixels look the same over there.
    auto requestReferenceOnPage(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId elementId,
        annotation::PageId pageId
    ) -> void;

    // Commits a finished canvas drag as one undo entry, on release rather than
    // per mouse-move. A page context (from referenceContext) routes a search-range
    // edit to that page's reference, so editing the range while viewing one page
    // leaves every other page's range untouched; without it the edit writes the
    // element's own default range.
    auto editSelectedRectOnRelease(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId recognizerId,
        std::optional<annotation::PageId> pageContext,
        PixelRect const& editedRect
    ) -> void;
}
