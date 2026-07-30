#include "authoring-actions.hpp"

#include "authoring-edit.hpp"
#include "edit-page.hpp"
#include "model-check-view.hpp"
#include "preview.hpp"
#include "project-persistence.hpp"
#include "workbench-app.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <algorithm>
#include <cmath>
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
        annotation::ElementId id
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

    namespace
    {
        // The inclusive basis-point ceiling a threshold may hold: 10000 is 100.00
        // percent. The design fixes the range at [0, 10000] (§1.4).
        constexpr auto k_thresholdMaxBasisPoints = int64{10'000};
    }

    auto thresholdPercentFromBasisPoints(uint32 basisPoints) noexcept -> float
    {
        return static_cast<float>(basisPoints) / 100.0F;
    }

    auto thresholdBasisPointsFromPercent(float percent) noexcept -> uint32
    {
        // Rounded in double so a percent that is not exactly representable in
        // float (99.99 is not) still recovers the basis points it came from.
        auto const scaled = std::llround(static_cast<double>(percent) * 100.0);
        auto const clamped = std::clamp(
            static_cast<int64>(scaled),
            int64{0},
            k_thresholdMaxBasisPoints
        );
        return static_cast<uint32>(clamped);
    }

    auto requestDuplicateElement(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId id
    ) -> void
    {
        auto const newId = annotation::ElementId{mintResourceId()};
        auto duplicated  = duplicateElement(
            state.draft(),
            DuplicateElementSpec{
                .sourceElementId = id,
                .newElementId    = newId,
            }
        );
        if (!duplicated)
        {
            ui.report(
                LogSeverity::Error,
                std::format(
                    "duplicate failed: {}",
                    toString(duplicated.error())
                )
            );
            return;
        }
        // The copy is authored on the same screen as the original, so the
        // selection follows there once the edit lands rather than leaving its
        // rectangles drawn over whatever the canvas happens to show.
        requestEditSelecting(
            ui,
            std::move(duplicated->draft),
            std::format("duplicated as \"{}\"", duplicated->name),
            newId,
            sourceOfRecognizer(state, id)
        );
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
        annotation::ElementId recognizerId,
        std::optional<annotation::SourceId> sourceId,
        LogSeverity severity
    ) -> void
    {
        if (ui.pendingEdit.has_value())
        {
            return;
        }
        ui.pendingEdit = PanelUiState::PendingEdit{
            .draft       = std::move(draft),
            .description = std::move(description),
            .severity    = severity,
            .selection   = AppState::Selection{
                AppState::Selection::Element{
                    .recognizerId = recognizerId,
                    .shownScreen  = sourceId,
                },
            },
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
            ui.report(
                LogSeverity::Error,
                "edit rejected: the project changed while this edit was open; "
                "reopen it and try again"
            );
            return;
        }

        auto const applied = state.applyEdit(request.draft);
        if (!applied)
        {
            ui.report(
                LogSeverity::Error,
                std::format(
                    "edit rejected: {}",
                    toString(applied.error())
                )
            );
            return;
        }
        if (*applied)
        {
            // A check in flight is answering a question about the document
            // this edit just replaced, so its verdict would be attached to
            // marks that have since moved.
            ui.modelCheck.discard();
            ui.report(request.severity, request.description);
            if (request.selection.has_value())
            {
                state.select(*request.selection);
            }
        }
    }

    auto requestToolbarCommand(PanelUiState& ui, ToolbarCommand command) -> void
    {
        if (ui.pendingToolbarCommand.has_value())
        {
            return;
        }
        ui.pendingToolbarCommand = command;
    }

    namespace
    {
        // Save the current document and regenerate the runtime project, reporting
        // the outcome. Lifted verbatim from the former Actions button so the
        // toolbar's queued command behaves identically; it runs after
        // applyPendingEdit, so it saves the document this frame's edit produced.
        auto saveAndGenerate(AppState& state, PanelUiState& ui) -> void
        {
            auto const assets = state.compilerSourceAssets();
            if (!assets)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("save failed: {}", toString(assets.error()))
                );
                return;
            }
            auto const status = saveAndGenerateAuthoringProject(
                state.projectRoot(),
                state.document(),
                *assets
            );
            if (!status)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format("save failed: {}", toString(status.error()))
                );
                return;
            }
            state.markSaved();
            ui.report(LogSeverity::Info, "saved and generated");
        }

        // The catalog-count line an undo or redo leaves, so the author sees what
        // the reverted document now holds.
        [[nodiscard]]
        auto historyMoveLine(
            AppState const& state,
            std::string_view verb
        ) -> std::string
        {
            return std::format(
                "{}: {} recognizers, {} pages",
                verb,
                state.document().catalog().recognizers().size(),
                state.document().catalog().pages().size()
            );
        }
    }

    auto dispatchToolbarCommand(AppState& state, PanelUiState& ui) -> void
    {
        if (!ui.pendingToolbarCommand.has_value())
        {
            return;
        }
        auto const command = *std::exchange(ui.pendingToolbarCommand, std::nullopt);
        switch (command)
        {
        case ToolbarCommand::SaveAndGenerate:
            saveAndGenerate(state, ui);
            return;
        case ToolbarCommand::Undo:
            if (state.undo())
            {
                ui.report(LogSeverity::Info, historyMoveLine(state, "undo"));
            }
            return;
        case ToolbarCommand::Redo:
            if (state.redo())
            {
                ui.report(LogSeverity::Info, historyMoveLine(state, "redo"));
            }
            return;
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
            ui.report(
                LogSeverity::Error,
                std::format(
                    "delete rejected: {}",
                    toString(deleted.error())
                )
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
        annotation::ElementId id
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

    auto shownPageForScreen(
        AppState const& state,
        annotation::SourceId shownScreen,
        std::optional<annotation::PageId> selectionPage
    ) -> std::optional<annotation::PageId>
    {
        // Precedence: the page the element was selected under wins; only without
        // one does it fall back to the page that claims the shown screen (the
        // inverse of claimedScreen -- a screen resolves to exactly one page).
        if (selectionPage.has_value())
        {
            return selectionPage;
        }
        for (auto const& regression : state.document().regressions())
        {
            auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                &regression.expectation()
            );
            if (p_resolved != nullptr && regression.sourceId() == shownScreen)
            {
                return p_resolved->pageId;
            }
        }
        return std::nullopt;
    }

    auto placementContext(
        AppState const& state,
        annotation::ElementId id,
        annotation::SourceId shownScreen,
        std::optional<annotation::PageId> selectionPage
    ) -> std::optional<PlacementContext>
    {
        auto const pageContext = shownPageForScreen(
            state,
            shownScreen,
            selectionPage
        );
        if (!pageContext.has_value())
        {
            return std::nullopt;
        }

        // Only an interactive region carries a per-page placement the canvas can
        // edit here: an anchor joins its page through the signature, and an info
        // region's default range is left to the properties panel.
        auto const* definition = state.document().catalog().findRecognizer(id);
        if (
            definition == nullptr
            || definition->annotationType()
                != annotation::AnnotationType::ActionTarget
        )
        {
            return std::nullopt;
        }

        for (auto const& placement : state.document().placements())
        {
            if (
                placement.pageId == *pageContext
                && placement.elementId == id
            )
            {
                return PlacementContext{
                    .page      = *pageContext,
                    .searchRoi = placement.searchRoi,
                };
            }
        }
        return std::nullopt;
    }

    auto isRegionShared(
        AppState const& state,
        annotation::ElementId id
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
        annotation::ElementId id,
        bool shared
    ) -> void
    {
        auto marked = setRegionShared(state.draft(), id, shared);
        if (!marked)
        {
            ui.report(
                LogSeverity::Error,
                std::format(
                    "{} failed: {}",
                    shared ? "sharing" : "unsharing",
                    toString(marked.error())
                )
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

    auto elementColourKey(
        AppState const& state,
        annotation::ElementId id
    ) -> std::optional<annotation::ColourKey>
    {
        auto const* p_element = state.document().findElement(id);
        return p_element == nullptr
            ? std::nullopt
            : p_element->colourKey();
    }

    auto requestElementColourKey(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId id,
        std::optional<annotation::ColourKey> colourKey
    ) -> void
    {
        auto keyed = setElementColourKey(state.draft(), id, colourKey);
        if (!keyed)
        {
            ui.report(
                LogSeverity::Error,
                std::format(
                    "colour key change failed: {}",
                    toString(keyed.error())
                )
            );
            return;
        }
        requestEdit(
            ui,
            *std::move(keyed),
            colourKey
                ? std::format(
                    "colour key set to {}, {}, {} within {}",
                    colourKey->red(),
                    colourKey->green(),
                    colourKey->blue(),
                    colourKey->tolerance()
                )
                : std::string{"colour key removed; the whole box is compared again"}
        );
    }

    auto requestScreenExpectation(
        AppState& state,
        PanelUiState& ui,
        annotation::SourceId source,
        PagelessExpectation expectation
    ) -> void
    {
        auto recorded = recordScreenExpectation(
            state.draft(),
            ScreenExpectationSpec{
                .regressionId = annotation::RegressionId{mintResourceId()},
                .sourceId     = source,
                .expectation  = expectation,
            }
        );
        if (!recorded)
        {
            ui.report(
                LogSeverity::Error,
                std::format(
                    "recording the screen failed: {}",
                    toString(recorded.error())
                )
            );
            return;
        }
        requestEdit(
            ui,
            *std::move(recorded),
            std::format(
                "screen {} recorded as {}",
                shortId(source.value()),
                expectation == PagelessExpectation::Unknown
                    ? "none of the pages"
                    : "ambiguous"
            )
        );
    }

    auto requestSharedRegionOnPage(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId shareFrom,
        annotation::PageId pageId
    ) -> void
    {
        auto draft         = state.draft();
        auto const* origin = findEditableRecognizer(draft, shareFrom);
        if (origin == nullptr)
        {
            ui.report(
                LogSeverity::Error,
                "that region is no longer in the project"
            );
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
            ui.report(
                LogSeverity::Error,
                std::format(
                    "share failed: {}",
                    toString(shared.error())
                )
            );
            return;
        }

        // The new placement searches the region the element already uses, so
        // scoring the element against the target screen is scoring the placement
        // the drop just created.
        auto verdict = std::string{};

        // A placement that scores as not matching on its target screen is a
        // done-but-degraded outcome, so its otherwise-successful message is
        // reported at Warning rather than Info.
        auto severity = LogSeverity::Info;
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
                    if (!scored->hit)
                    {
                        severity = LogSeverity::Warning;
                    }
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
            targetScreen,
            severity
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
        annotation::ElementId id,
        std::optional<annotation::SourceId> preferredScreen,
        std::optional<annotation::PageId> pageContext
    ) -> void
    {
        auto const screen = preferredScreen.has_value()
            ? preferredScreen
            : sourceOfRecognizer(state, id);
        // A nullopt shown screen makes select() inherit the currently shown one,
        // so an element with no resolvable screen stays over the current image
        // rather than clearing it -- as the prior two-setter form did.
        state.select(
            AppState::Selection::Element{
                .recognizerId = id,
                .shownScreen  = screen,
                .pageContext  = pageContext,
            }
        );
    }

    auto editSelectedRectOnRelease(
        AppState& state,
        PanelUiState& ui,
        annotation::ElementId recognizerId,
        std::optional<annotation::PageId> pageContext,
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
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "template change rejected: {}",
                        toString(retemplated.error())
                    )
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
        else if (pageContext.has_value())
        {
            // With a page context the drag edits that page's placement, not the
            // element's shared default, so the range set here moves on this page
            // alone. Routed through EditPage so the placement write and the
            // one-commit-per-frame guard stay in one place.
            auto opened = EditPage::open(state, *pageContext);
            if (!opened)
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "search roi change rejected: {}",
                        toString(opened.error())
                    )
                );
            }
            else if (
                auto region = opened->region(recognizerId);
                !region
            )
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "search roi change rejected: {}",
                        toString(region.error())
                    )
                );
            }
            else if (
                auto const set = region->setSearchRoi(editedRect);
                !set
            )
            {
                ui.report(
                    LogSeverity::Error,
                    std::format(
                        "search roi change rejected: {}",
                        toString(set.error())
                    )
                );
            }
            else
            {
                std::move(*opened).commit(
                    ui,
                    std::format(
                        "search roi set to {} on page \"{}\"",
                        geometry,
                        pageName(state, *pageContext)
                    )
                );
            }
        }
        else
        {
            // No page context: this writes the element's own default search
            // range, which seeds new placements and every page that has not
            // refined its own.
            auto draft       = state.draft();
            auto* recognizer = findEditableRecognizer(draft, recognizerId);
            if (recognizer != nullptr)
            {
                recognizer->searchRoi = editedRect;
                requestEdit(
                    ui,
                    std::move(draft),
                    std::format("default search range set to {}", geometry)
                );
            }
        }

        ui.dragTarget = PanelUiState::CanvasDragTarget::None;
        ui.dragGrip.reset();
        ui.dragStartRect.reset();
    }
}
