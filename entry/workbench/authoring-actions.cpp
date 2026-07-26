#include "authoring-actions.hpp"

#include "authoring-edit.hpp"
#include "model-check-view.hpp"
#include "preview.hpp"
#include "app/workbench-app.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::workbench
{
    [[nodiscard]]
    auto shortId(annotation::ResourceId const& id) -> std::string
    {
        return id.toString().substr(0, 8);
    }

    [[nodiscard]]
    auto findEditableRecognizer(
        AuthoringDraft& draft,
        annotation::RecognizerId id
    ) -> EditableRecognizer*
    {
        auto const found = std::ranges::find(
            draft.recognizers,
            id,
            &EditableRecognizer::id
        );
        if (found == draft.recognizers.end())
        {
            return nullptr;
        }
        return &*found;
    }

    [[nodiscard]]
    auto pageName(AppState const& state, annotation::PageId id) -> std::string
    {
        auto const* page = state.document().catalog().findPage(id);
        if (page != nullptr)
        {
            return page->name().value();
        }
        return shortId(id.value());
    }

    // Names a deletion and the neighbours it had to edit, so a withdrawal or
    // a discarded regression case is stated rather than discovered later.
    [[nodiscard]]
    auto deletionSummary(
        std::string_view what,
        DeletedEntity const& deleted
    ) -> std::string
    {
        auto summary = std::format("deleted {}", what);
        if (deleted.withdrawnRoles > 0U)
        {
            summary += std::format(
                "; withdrew it from {} page {}",
                deleted.withdrawnRoles,
                deleted.withdrawnRoles == 1U ? "signature" : "signatures"
            );
        }
        if (deleted.clearedAuthorizations > 0U)
        {
            summary += std::format(
                "; cleared it from {} recognizer {}",
                deleted.clearedAuthorizations,
                deleted.clearedAuthorizations == 1U
                    ? "authorization"
                    : "authorizations"
            );
        }
        if (deleted.removedRegressions > 0U)
        {
            summary += std::format(
                "; removed {} regression {}",
                deleted.removedRegressions,
                deleted.removedRegressions == 1U ? "case" : "cases"
            );
        }
        return summary;
    }

    // Names the type change and everything the conversion had to repair with
    // it, so an authorization the author did not ask for and a field the
    // conversion could not keep are both stated rather than discovered later.
    [[nodiscard]]
    auto retypeSummary(
        AppState const& state,
        RetypedRecognizer const& retyped,
        char const* typeName
    ) -> std::string
    {
        auto summary = std::format("type set to {}", typeName);
        if (auto const page = retyped.authorizedPage)
        {
            summary += std::format(
                "; authorized page \"{}\"",
                pageName(state, *page)
            );
        }
        if (retyped.withdrawnRoles > 0U)
        {
            summary += std::format(
                "; withdrew from {} page {}",
                retyped.withdrawnRoles,
                retyped.withdrawnRoles == 1U ? "signature" : "signatures"
            );
        }
        if (retyped.clearedAuthorizations > 0U)
        {
            summary += std::format(
                "; cleared {} page {}",
                retyped.clearedAuthorizations,
                retyped.clearedAuthorizations == 1U
                    ? "authorization"
                    : "authorizations"
            );
        }
        if (retyped.clearedClick)
        {
            summary += "; cleared the default click";
        }
        return summary;
    }

    // Queues an edited draft for the end of the frame instead of committing it
    // where the widget was handled; see PendingEdit for why a mid-draw commit
    // is unsafe. Every request describes its edit, because the description
    // becomes the status line and the status line is what the operation log
    // records; an edit that left no trace there is one nobody can reconstruct
    // afterwards. A frame carries one request: a second would have been built
    // against the same document as the first and would silently drop it, and
    // only a widget deactivating in the same frame as another's click can
    // produce one, so the first request wins and the second click is retried
    // by the user on the next frame.
    auto requestEdit(
        PanelUiState& ui,
        AuthoringDraft draft,
        std::string description
    ) -> void
    {
        if (ui.pendingEdit.has_value())
        {
            return;
        }
        ui.pendingEdit = PanelUiState::PendingEdit{
            .draft       = std::move(draft),
            .description = std::move(description),
        };
    }

    // Queues an edit that creates a recognizer, so the selection follows it
    // to the image its rectangles were drawn on -- but only once the edit is
    // known to have landed.
    auto requestEditSelecting(
        PanelUiState& ui,
        AuthoringDraft draft,
        std::string description,
        annotation::RecognizerId recognizerId,
        std::optional<annotation::SourceId> sourceId
    ) -> void
    {
        if (ui.pendingEdit.has_value())
        {
            return;
        }
        ui.pendingEdit = PanelUiState::PendingEdit{
            .draft            = std::move(draft),
            .description      = std::move(description),
            .selectRecognizer = recognizerId,
            .selectSource     = sourceId,
        };
    }

    // Commits the frame's queued edit, if any, and states the outcome on the
    // status line: the requester's description when the document changed, the
    // build's rejection when it was refused. An edit that changes nothing
    // leaves the line alone.
    auto applyPendingEdit(AppState& state, PanelUiState& ui) -> void
    {
        if (!ui.pendingEdit.has_value())
        {
            return;
        }
        auto const request = *std::exchange(ui.pendingEdit, std::nullopt);

        // A draft carrying a base revision was built by an EditPage against a
        // specific history version. If the author has since undone or redone,
        // the version it edits is gone, and committing it would resurrect state
        // they left behind. Refuse it visibly rather than apply it -- the edit
        // is discarded either way, since the request was already taken above.
        if (
            request.baseRevision.has_value()
            && *request.baseRevision != state.revision()
        )
        {
            ui.statusLine =
                "edit rejected: the project changed while this edit was open; "
                "reopen it and try again";
            return;
        }

        auto const applied = state.applyEdit(request.draft);
        if (!applied)
        {
            ui.statusLine = std::format(
                "edit rejected: {}",
                toString(applied.error())
            );
            return;
        }
        if (*applied)
        {
            // A check in flight is answering a question about the document
            // this edit just replaced, so its verdict would be attached to
            // marks that have since moved.
            ui.modelCheck.discard();
            ui.statusLine = request.description;
            if (request.selectRecognizer.has_value())
            {
                state.setSelectedRecognizerId(*request.selectRecognizer);
            }
            if (request.selectSource.has_value())
            {
                state.setSelectedSourceId(*request.selectSource);
            }
        }
    }

    // Queues a deletion, reporting a refusal immediately. Deletions are
    // refused rather than cascaded when the cascade would reach something
    // only the author can decide about.
    auto requestDeletion(
        PanelUiState& ui,
        Result<DeletedEntity> deleted,
        std::string_view what
    ) -> void
    {
        if (!deleted)
        {
            ui.statusLine = std::format(
                "delete rejected: {}",
                toString(deleted.error())
            );
            return;
        }
        auto description = deletionSummary(what, *deleted);
        requestEdit(ui, std::move(deleted->draft), std::move(description));
    }

    // The source a recognizer was authored against, so a selection can follow
    // the recognizer to the image its rectangles are meaningful on.
    [[nodiscard]]
    auto sourceOfRecognizer(
        AppState const& state,
        annotation::RecognizerId id
    ) -> std::optional<annotation::SourceId>
    {
        for (auto const& relationship : state.document().recognizerSources())
        {
            if (relationship.recognizerId == id)
            {
                return relationship.sourceId;
            }
        }
        return std::nullopt;
    }

    // The screen explicitly recorded as this page, if any. Distinct from
    // pageSampleSource, which falls back to an inference when nothing is
    // recorded: this answers whether the statement exists at all.
    [[nodiscard]]
    auto claimedScreen(
        AppState const& state,
        annotation::PageId pageId
    ) -> std::optional<annotation::SourceId>
    {
        for (auto const& regression : state.document().regressions())
        {
            auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                &regression.expectation()
            );
            if (p_resolved != nullptr && p_resolved->pageId == pageId)
            {
                return regression.sourceId();
            }
        }
        return std::nullopt;
    }

    auto isRegionShared(
        AppState const& state,
        annotation::RecognizerId id
    ) -> bool
    {
        auto const found = std::ranges::find(
            state.document().recognizerSources(),
            id,
            &annotation::AuthoringRecognizerSource::recognizerId
        );
        return found != state.document().recognizerSources().end()
            && found->shared;
    }

    auto requestRegionShared(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId id,
        bool shared
    ) -> void
    {
        auto marked = setRegionShared(state.draft(), id, shared);
        if (!marked)
        {
            ui.statusLine = std::format(
                "{} failed: {}",
                shared ? "sharing" : "unsharing",
                toString(marked.error())
            );
            return;
        }
        requestEdit(
            ui,
            *std::move(marked),
            shared
                ? "marked reusable; drag it onto another page from Shared regions"
                : std::string{"no longer reusable"}
        );
    }

    auto requestSharedRegionOnPage(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId shareFrom,
        annotation::PageId pageId
    ) -> void
    {
        auto draft         = state.draft();
        auto const* origin = findEditableRecognizer(draft, shareFrom);
        if (origin == nullptr)
        {
            ui.statusLine = "that region is no longer in the project";
            return;
        }

        auto const targetScreen = claimedScreen(state, pageId);
        auto shared             = shareRegionOnPage(
            state.draft(),
            SharedRegionSpec{
                .elementId = shareFrom,
                .pageId    = pageId,
                .searchRoi = origin->searchRoi,
            }
        );
        if (!shared)
        {
            ui.statusLine = std::format(
                "share failed: {}",
                toString(shared.error())
            );
            return;
        }

        // The new placement searches the region the element already uses, so
        // scoring the element against the target screen is scoring the placement
        // the drop just created.
        auto verdict = std::string{};
        if (targetScreen.has_value())
        {
            auto const assets = state.compilerSourceAssets();
            if (assets.has_value())
            {
                auto const scored = scoreRegionOnScreen(
                    state.document(),
                    *assets,
                    shareFrom,
                    *targetScreen,
                    annotation::RecognitionPolicy{
                        .maximumPixelComparisons = k_recognitionComparisonBudget,
                    }
                );
                if (scored.has_value())
                {
                    verdict = scored->hit
                        ? std::format(
                            "; it matches there, using {} of its budget",
                            budgetPercentText(scored->sadScore, scored->maximumSad)
                        )
                        : std::format(
                            "; WARNING it does not match there ({} of budget) -- "
                            "these pixels look different on that screen",
                            budgetPercentText(scored->sadScore, scored->maximumSad)
                        );
                }
            }
        }

        requestEditSelecting(
            ui,
            std::move(shared->draft),
            std::format(
                "\"{}\" placed on page \"{}\"{}",
                shared->name,
                pageName(state, pageId),
                verdict
            ),
            shareFrom,
            targetScreen
        );
    }

    // Selects a recognizer and shows it over a screen it is meaningful on.
    //
    // Which screen that is depends on where the author clicked. A member
    // picked out of a page group is shown over that page's screen, because
    // the search region being edited is a region of that screen. Everywhere
    // else the recognizer follows to the image its template was cut from.
    //
    // For a shared element the two are different images, and that is the
    // point: the template belongs to the screen it was drawn on while the
    // search region belongs to the page being looked at. The canvas draws the
    // template box dimmed and refuses to edit it whenever the screen on
    // display is not the one it was cut from.
    auto selectRecognizer(
        AppState& state,
        annotation::RecognizerId id,
        std::optional<annotation::SourceId> preferredScreen
    ) -> void
    {
        state.setSelectedRecognizerId(id);
        auto const screen = preferredScreen.has_value()
            ? preferredScreen
            : sourceOfRecognizer(state, id);
        if (screen.has_value())
        {
            state.setSelectedSourceId(*screen);
        }
    }

    auto editSelectedRectOnRelease(
        AppState& state,
        PanelUiState& ui,
        annotation::RecognizerId recognizerId,
        PixelRect const& editedRect
    ) -> void
    {
        // Committing only on release keeps a whole drag gesture to a single
        // undo entry instead of one per moved pixel.
        auto const isTemplate = (
            ui.dragTarget == PanelUiState::CanvasDragTarget::TemplateRect
        );
        auto const geometry = std::format(
            "{},{} {}x{}",
            editedRect.x(),
            editedRect.y(),
            editedRect.width(),
            editedRect.height()
        );

        if (isTemplate)
        {
            // An element is one thing placed on N pages, so correcting its
            // template corrects it everywhere at once. Each placement keeps its
            // own detection range, which the moved template must still fit.
            auto retemplated = setElementTemplateRect(
                state.draft(),
                recognizerId,
                editedRect
            );
            if (!retemplated)
            {
                ui.statusLine = std::format(
                    "template change rejected: {}",
                    toString(retemplated.error())
                );
            }
            else
            {
                auto description = std::format("template rect set to {}", geometry);
                if (retemplated->otherPlacements > 1U)
                {
                    description += std::format(
                        "; searched on {} pages, all updated",
                        retemplated->otherPlacements
                    );
                }
                requestEdit(
                    ui,
                    std::move(retemplated->draft),
                    std::move(description)
                );
            }
        }
        else
        {
            auto draft       = state.draft();
            auto* recognizer = findEditableRecognizer(draft, recognizerId);
            if (recognizer != nullptr)
            {
                recognizer->searchRoi = editedRect;
                requestEdit(
                    ui,
                    std::move(draft),
                    std::format("search roi set to {}", geometry)
                );
            }
        }

        ui.dragTarget = PanelUiState::CanvasDragTarget::None;
        ui.dragGrip.reset();
        ui.dragStartRect.reset();
    }
}
