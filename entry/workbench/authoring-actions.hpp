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

    // Parks a validated draft for this frame's single commit.
    auto requestEdit(
        PanelUiState& ui,
        AuthoringDraft draft,
        std::string description
    ) -> void;

    // As requestEdit, and selects the entity once the edit lands. Selecting
    // before it lands would leave the selection pointing at an id a rejected
    // edit never added.
    auto requestEditSelecting(
        PanelUiState& ui,
        AuthoringDraft draft,
        std::string description,
        annotation::RecognizerId recognizerId,
        std::optional<annotation::SourceId> sourceId
    ) -> void;

    // Commits this frame's parked edit, if any. Called once per frame, after the
    // panels that borrow into the document and before the one that mutates it.
    auto applyPendingEdit(AppState& state, PanelUiState& ui) -> void;

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

    // The screen a recognizer was authored against, if the document records one.
    [[nodiscard]]
    auto sourceOfRecognizer(
        AppState const& state,
        annotation::RecognizerId id
    ) -> std::optional<annotation::SourceId>;

    // Creates a page, its anchor, and the regression case that records which
    // screen the page stands for, as one draft.
    auto requestNewPage(AppState& state, PanelUiState& ui) -> void;

    // Adds a recognizer to a page's required or forbidden set, authoring the
    // recognizer against the given screen.
    auto requestNewPageMember(
        AppState& state,
        PanelUiState& ui,
        annotation::PageId pageId,
        annotation::SourceId sourceId,
        PageMemberKind kind
    ) -> void;

    // The screen a page's regression case claims, if it has one.
    [[nodiscard]]
    auto claimedScreen(
        AppState const& state,
        annotation::PageId pageId
    ) -> std::optional<annotation::SourceId>;

    // Records that a screen is an example of a page, which is what gives the
    // model check something to check that page's resolution against.
    auto requestScreenClaim(
        AppState& state,
        PanelUiState& ui,
        annotation::PageId pageId,
        annotation::SourceId sourceId
    ) -> void;

    // Selects a recognizer and follows to the screen it was authored against,
    // without which its rectangles are drawn over whatever image the canvas
    // happens to hold.
    auto selectRecognizer(
        AppState& state,
        annotation::RecognizerId id,
        std::optional<annotation::SourceId> preferredScreen
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
    // per mouse-move.
    auto editSelectedRectOnRelease(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId recognizerId,
        PixelRect const& editedRect
    ) -> void;
}
