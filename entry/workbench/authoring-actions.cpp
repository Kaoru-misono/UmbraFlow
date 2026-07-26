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
    namespace
    {
        // What a newly drawn recognizer starts as before the author drags it: a
        // box small enough to be legal at any project resolution, and the 90 %
        // similarity the annotation design takes as the default.
        constexpr auto k_startingTemplateExtent        = uint32{16};
        constexpr auto k_startingSimilarityBasisPoints = uint32{9'000};

        struct StartingRects final
        {
            PixelRect m_templateRect;
            PixelRect m_searchRoi;
        };

        [[nodiscard]]
        auto startingRects(
            annotation::ProjectFingerprint fingerprint
        ) -> Result<StartingRects>
        {
            auto const width  = fingerprint.width();
            auto const height = fingerprint.height();
            UF_TRY_VALUE(
                templateRect,
                PixelRect::create(
                    0U,
                    0U,
                    std::min<uint32>(k_startingTemplateExtent, width),
                    std::min<uint32>(k_startingTemplateExtent, height)
                )
            );
            UF_TRY_VALUE(searchRoi, PixelRect::create(0U, 0U, width, height));
            return StartingRects{
                .m_templateRect = templateRect,
                .m_searchRoi    = searchRoi,
            };
        }
    }

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
            draft.m_recognizers,
            id,
            &EditableRecognizer::m_id
        );
        if (found == draft.m_recognizers.end())
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
        if (deleted.m_withdrawnRoles > 0U)
        {
            summary += std::format(
                "; withdrew it from {} page {}",
                deleted.m_withdrawnRoles,
                deleted.m_withdrawnRoles == 1U ? "signature" : "signatures"
            );
        }
        if (deleted.m_clearedAuthorizations > 0U)
        {
            summary += std::format(
                "; cleared it from {} recognizer {}",
                deleted.m_clearedAuthorizations,
                deleted.m_clearedAuthorizations == 1U
                    ? "authorization"
                    : "authorizations"
            );
        }
        if (deleted.m_removedRegressions > 0U)
        {
            summary += std::format(
                "; removed {} regression {}",
                deleted.m_removedRegressions,
                deleted.m_removedRegressions == 1U ? "case" : "cases"
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
        if (auto const page = retyped.m_authorizedPage)
        {
            summary += std::format(
                "; authorized page \"{}\"",
                pageName(state, *page)
            );
        }
        if (retyped.m_withdrawnRoles > 0U)
        {
            summary += std::format(
                "; withdrew from {} page {}",
                retyped.m_withdrawnRoles,
                retyped.m_withdrawnRoles == 1U ? "signature" : "signatures"
            );
        }
        if (retyped.m_clearedAuthorizations > 0U)
        {
            summary += std::format(
                "; cleared {} page {}",
                retyped.m_clearedAuthorizations,
                retyped.m_clearedAuthorizations == 1U
                    ? "authorization"
                    : "authorizations"
            );
        }
        if (retyped.m_clearedClick)
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
        if (ui.m_pendingEdit.has_value())
        {
            return;
        }
        ui.m_pendingEdit = PendingEdit{
            .m_draft       = std::move(draft),
            .m_description = std::move(description),
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
        if (ui.m_pendingEdit.has_value())
        {
            return;
        }
        ui.m_pendingEdit = PendingEdit{
            .m_draft            = std::move(draft),
            .m_description      = std::move(description),
            .m_selectRecognizer = recognizerId,
            .m_selectSource     = sourceId,
        };
    }

    // Commits the frame's queued edit, if any, and states the outcome on the
    // status line: the requester's description when the document changed, the
    // build's rejection when it was refused. An edit that changes nothing
    // leaves the line alone.
    auto applyPendingEdit(AppState& state, PanelUiState& ui) -> void
    {
        if (!ui.m_pendingEdit.has_value())
        {
            return;
        }
        auto const request = *std::exchange(ui.m_pendingEdit, std::nullopt);

        auto const applied = state.applyEdit(request.m_draft);
        if (!applied)
        {
            ui.m_statusLine = std::format(
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
            ui.m_modelCheck.discard();
            ui.m_statusLine = request.m_description;
            if (request.m_selectRecognizer.has_value())
            {
                state.setSelectedRecognizerId(*request.m_selectRecognizer);
            }
            if (request.m_selectSource.has_value())
            {
                state.setSelectedSourceId(*request.m_selectSource);
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
            ui.m_statusLine = std::format(
                "delete rejected: {}",
                toString(deleted.error())
            );
            return;
        }
        auto description = deletionSummary(what, *deleted);
        requestEdit(ui, std::move(deleted->m_draft), std::move(description));
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
            if (relationship.m_recognizerId == id)
            {
                return relationship.m_sourceId;
            }
        }
        return std::nullopt;
    }

    // Turns the selected screen into a page: the anchor that identifies it,
    // the page requiring that anchor, and the record that this screen is
    // expected to resolve to it, all in one edit.
    auto requestNewPage(AppState& state, PanelUiState& ui) -> void
    {
        auto const source = state.selectedSourceId();
        if (!source.has_value())
        {
            ui.m_statusLine = "select a screen first";
            return;
        }

        auto draft        = state.draft();
        auto const rects  = startingRects(draft.m_fingerprint);
        if (!rects)
        {
            ui.m_statusLine = std::format(
                "new page failed: {}",
                toString(rects.error())
            );
            return;
        }

        auto const anchorId = annotation::RecognizerId{mintResourceId()};
        auto created        = createPageFromSource(
            std::move(draft),
            NewPageSpec{
                .m_pageId   = annotation::PageId{mintResourceId()},
                .m_anchorId = anchorId,
                .m_regressionId          = annotation::RegressionId{
                    mintResourceId()
                },
                .m_sourceId              = *source,
                .m_templateRect          = rects->m_templateRect,
                .m_searchRoi             = rects->m_searchRoi,
                .m_similarityBasisPoints = k_startingSimilarityBasisPoints,
            }
        );
        if (!created)
        {
            ui.m_statusLine = std::format(
                "new page failed: {}",
                toString(created.error())
            );
            return;
        }

        auto description = std::format(
            "added page \"{}\" with \"{}\" identifying it; "
            "drag its box over a mark unique to this screen",
            created->m_pageName,
            created->m_anchorName
        );
        requestEditSelecting(
            ui,
            std::move(created->m_draft),
            std::move(description),
            anchorId,
            *source
        );
    }

    // Adds one member to a page, typed and linked by which button was
    // pressed. The author never names the annotation type or fills in a role
    // or an authorization: both follow from the question the button asks.
    auto requestNewPageMember(
        AppState& state,
        PanelUiState& ui,
        annotation::PageId pageId,
        annotation::SourceId sourceId,
        PageMemberKind kind
    ) -> void
    {
        auto draft       = state.draft();
        auto const rects = startingRects(draft.m_fingerprint);
        if (!rects)
        {
            ui.m_statusLine = std::format(
                "add failed: {}",
                toString(rects.error())
            );
            return;
        }

        auto const recognizerId = annotation::RecognizerId{mintResourceId()};
        auto added              = addPageMember(
            std::move(draft),
            PageMemberSpec{
                .m_recognizerId = recognizerId,
                .m_pageId       = pageId,
                .m_sourceId     = sourceId,
                .m_templateRect          = rects->m_templateRect,
                .m_searchRoi             = rects->m_searchRoi,
                .m_similarityBasisPoints = k_startingSimilarityBasisPoints,
                .m_kind                  = kind,
            }
        );
        if (!added)
        {
            ui.m_statusLine = std::format(
                "add failed: {}",
                toString(added.error())
            );
            return;
        }

        auto description = std::format(
            "added \"{}\" as {}; drag its box over the {}",
            added->m_name,
            kind == PageMemberKind::Anchor
                ? "a mark identifying this page"
                : "an interactive region on this page",
            kind == PageMemberKind::Anchor ? "mark" : "region"
        );
        requestEditSelecting(
            ui,
            std::move(added->m_draft),
            std::move(description),
            recognizerId,
            sourceId
        );
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
            if (p_resolved != nullptr && p_resolved->m_pageId == pageId)
            {
                return regression.sourceId();
            }
        }
        return std::nullopt;
    }

    auto requestScreenClaim(
        AppState& state,
        PanelUiState& ui,
        annotation::PageId pageId,
        annotation::SourceId sourceId
    ) -> void
    {
        auto claimed = claimScreenForPage(
            state.draft(),
            ScreenClaimSpec{
                .m_regressionId = annotation::RegressionId{mintResourceId()},
                .m_sourceId     = sourceId,
                .m_pageId       = pageId,
            }
        );
        if (!claimed)
        {
            ui.m_statusLine = std::format(
                "recording the screen failed: {}",
                toString(claimed.error())
            );
            return;
        }
        requestEdit(
            ui,
            *std::move(claimed),
            std::format(
                "screen {} recorded as page \"{}\"",
                shortId(sourceId.value()),
                pageName(state, pageId)
            )
        );
    }

    auto isRegionShared(
        AppState const& state,
        annotation::RecognizerId id
    ) -> bool
    {
        auto const found = std::ranges::find(
            state.document().recognizerSources(),
            id,
            &annotation::AuthoringRecognizerSource::m_recognizerId
        );
        return found != state.document().recognizerSources().end()
            && found->m_shared;
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
            ui.m_statusLine = std::format(
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
            ui.m_statusLine = "that region is no longer in the project";
            return;
        }

        auto const targetScreen = claimedScreen(state, pageId);
        auto const newId        = annotation::RecognizerId{mintResourceId()};
        auto shared             = shareRegionOnPage(
            state.draft(),
            SharedRegionSpec{
                .m_recognizerId = newId,
                .m_shareFrom    = shareFrom,
                .m_pageId       = pageId,
                .m_searchRoi    = origin->m_searchRoi,
            }
        );
        if (!shared)
        {
            ui.m_statusLine = std::format(
                "share failed: {}",
                toString(shared.error())
            );
            return;
        }

        // Measured against the element being copied rather than the copy, which
        // does not exist until this edit lands. Both carry the same template and
        // the same region, so the score is the one the copy will have.
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
                        .m_maximumPixelComparisons = k_recognitionComparisonBudget,
                    }
                );
                if (scored.has_value())
                {
                    verdict = scored->m_hit
                        ? std::format(
                            "; it matches there, using {} of its budget",
                            budgetPercentText(scored->m_sadScore, scored->m_maximumSad)
                        )
                        : std::format(
                            "; WARNING it does not match there ({} of budget) -- "
                            "these pixels look different on that screen",
                            budgetPercentText(scored->m_sadScore, scored->m_maximumSad)
                        );
                }
            }
        }

        requestEditSelecting(
            ui,
            std::move(shared->m_draft),
            std::format(
                "\"{}\" added to page \"{}\" as \"{}\"{}",
                origin->m_name,
                pageName(state, pageId),
                shared->m_name,
                verdict
            ),
            newId,
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
            ui.m_dragTarget == CanvasDragTarget::TemplateRect
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
            // A shared element is drawn once, so correcting its template has
            // to correct it on every page it appears on. Each page keeps its
            // own detection range.
            auto retemplated = retemplateSharedRegion(
                state.draft(),
                recognizerId,
                editedRect
            );
            if (!retemplated)
            {
                ui.m_statusLine = std::format(
                    "template change rejected: {}",
                    toString(retemplated.error())
                );
            }
            else
            {
                auto description = std::format("template rect set to {}", geometry);
                if (retemplated->m_movedMembers > 0U)
                {
                    description += std::format(
                        "; moved it on {} other {}",
                        retemplated->m_movedMembers,
                        retemplated->m_movedMembers == 1U ? "page" : "pages"
                    );
                }
                requestEdit(
                    ui,
                    std::move(retemplated->m_draft),
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
                recognizer->m_searchRoi = editedRect;
                requestEdit(
                    ui,
                    std::move(draft),
                    std::format("search roi set to {}", geometry)
                );
            }
        }

        ui.m_dragTarget = CanvasDragTarget::None;
        ui.m_dragGrip.reset();
        ui.m_dragStartRect.reset();
    }
}
