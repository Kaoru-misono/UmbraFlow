#pragma once

#include "authoring-edit.hpp"
#include "panel-state.hpp"
#include "app/workbench-app.hpp"

#include <annotation/catalog.hpp>

#include <core/error/result.hpp>

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
        annotation::RecognizerId id
    ) -> EditableRecognizer*;

    // Says what retyping repaired as well as that it succeeded. An authorization
    // or a withdrawn page membership the author did not ask for must not be
    // silent, because the next thing they do depends on it.
    [[nodiscard]]
    auto retypeSummary(
        AppState const& state,
        RetypedRecognizer const& retyped,
        char const* typeName
    ) -> std::string;

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
    // same rectangles, threshold, click, kind, and placements. Reports a refusal
    // on the status line rather than committing a rejected edit.
    auto requestDuplicateElement(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId id
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
        annotation::RecognizerId recognizerId,
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

    // States what a deletion withdrew along the way, for the same reason
    // retypeSummary does.
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

    // The screen a recognizer was authored against, if the document records one.
    [[nodiscard]]
    auto sourceOfRecognizer(
        AppState const& state,
        annotation::RecognizerId id
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

    // The page whose placement of an interactive region the canvas edits when
    // that region is shown over a given screen, together with that placement's
    // per-page search ROI. Narrowed to the case the canvas can act on: the
    // recognizer is an interactive region and that page places it. Empty for an
    // anchor, or a region not placed on the resolved page, in which case the
    // canvas falls back to the element's own default range.
    //
    // The page is resolved by precedence: selectionPage -- the page an element
    // was selected under, from a page's member list -- wins outright; only
    // without one does the context fall back to the page that claims the shown
    // screen (the inverse of claimedScreen). This is what lets ROI editing on an
    // element selected under a page use that page even when the shown screen's
    // claim is missing or ambiguous.
    struct PlacementContext final
    {
        annotation::PageId page;
        PixelRect          searchRoi;
    };

    [[nodiscard]]
    auto placementContext(
        AppState const& state,
        annotation::RecognizerId id,
        annotation::SourceId shownScreen,
        std::optional<annotation::PageId> selectionPage = std::nullopt
    ) -> std::optional<PlacementContext>;

    // Selects a recognizer and follows to the screen it is meaningful on:
    // preferredScreen when the caller knows it (a member picked from a page's
    // group), otherwise the screen the template was cut from. pageContext is the
    // page the element was selected under, if any, which the canvas then prefers
    // when resolving the placement it edits. Without a resolvable screen the
    // element inherits the currently shown one so its rectangles are not drawn
    // over whatever image the canvas happens to hold.
    auto selectRecognizer(
        AppState& state,
        annotation::RecognizerId id,
        std::optional<annotation::SourceId> preferredScreen,
        std::optional<annotation::PageId> pageContext = std::nullopt
    ) -> void;

    // Whether the author marked this region reusable on other pages. The flag is
    // authoring-only, so it lives beside the recognizer rather than on it.
    [[nodiscard]]
    auto isRegionShared(
        AppState const& state,
        annotation::RecognizerId id
    ) -> bool;

    // Marks an interactive region reusable on other pages, or takes the mark off,
    // reporting a refusal rather than letting the checkbox snap back unexplained.
    auto requestRegionShared(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId id,
        bool shared
    ) -> void;

    // Puts a shared element on another page and says at once whether it is
    // actually there.
    //
    // The copy starts with the region the element already searches, not the whole
    // frame: the same element usually sits in the same place, so that is both the
    // better first guess and a search small enough to score on the spot. The
    // score is measured against the page being dropped onto, which is the only
    // screen the copy will ever be searched on, and it is the answer to the
    // question sharing cannot answer on its own -- whether these pixels look the
    // same over there.
    auto requestSharedRegionOnPage(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId shareFrom,
        annotation::PageId pageId
    ) -> void;

    // Commits a finished canvas drag as one undo entry, on release rather than
    // per mouse-move. A page context (from placementContext) routes a search-ROI
    // edit to that page's placement, so editing the range while viewing one page
    // leaves every other page's range untouched; without it the edit writes the
    // element's own default range.
    auto editSelectedRectOnRelease(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId recognizerId,
        std::optional<annotation::PageId> pageContext,
        PixelRect const& editedRect
    ) -> void;
}
