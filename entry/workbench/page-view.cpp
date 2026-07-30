#include "page-view.hpp"

#include "authoring-edit.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/capabilities.hpp>
#include <annotation/resource.hpp>

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto memberRow(
            EditableRecognizer const& recognizer,
            EditableReference const& reference
        ) -> PageView::MemberRow
        {
            auto templateRect = std::optional<PixelRect>{};
            if (auto const* p_variant = primaryVariant(recognizer))
            {
                templateRect = p_variant->templateRect;
            }
            auto clickOffset = std::optional<EditableTemplateOffset>{};
            if (auto const& interact = recognizer.capabilities.interact)
            {
                clickOffset = interact->clickOffset;
            }
            return PageView::MemberRow{
                .id           = recognizer.id,
                .name         = recognizer.name,
                .templateRect = templateRect,
                .searchRoiOnThisPage = reference.searchRoi.value_or(
                    recognizer.searchRoi
                ),
                .clickOffset = clickOffset,
                .holding     = reference.holding,
            };
        }

        // The screen a draft records as an example of this page, if any: the
        // source whose regression case is expected to resolve to it.
        [[nodiscard]]
        auto claimedScreenFor(
            AuthoringDraft const& draft,
            annotation::PageId id
        ) -> std::optional<annotation::SourceId>
        {
            for (auto const& regression : draft.regressions)
            {
                auto const* p_resolved =
                    std::get_if<annotation::ResolvedRegression>(
                        &regression.expectation
                    );
                if (p_resolved != nullptr && p_resolved->pageId == id)
                {
                    return regression.sourceId;
                }
            }
            return std::nullopt;
        }
    }

    auto PageView::of(
        AuthoringDraft const& draft,
        annotation::PageId id
    ) -> std::optional<PageView>
    {
        auto const page = std::ranges::find(
            draft.pages,
            id,
            &EditablePage::id
        );
        if (page == draft.pages.end())
        {
            return std::nullopt;
        }

        auto view = PageView{
            .id            = page->id,
            .name          = page->name,
            .claimedScreen = claimedScreenFor(draft, id),
        };

        // One pass over the page's references. A reference exercising several
        // capabilities puts the same row in several groups, which is the whole
        // point of a set: nothing an element does on this page is invisible, and
        // that is what once left a retyped region reachable only through undo.
        for (auto const& reference : draft.references)
        {
            if (reference.pageId != id)
            {
                continue;
            }
            auto const recognizer = std::ranges::find(
                draft.recognizers,
                reference.elementId,
                &EditableRecognizer::id
            );
            if (recognizer == draft.recognizers.end())
            {
                continue;
            }

            auto const row = memberRow(*recognizer, reference);
            if (auto const& identify = reference.exercised.identify)
            {
                switch (identify->role)
                {
                case annotation::SignatureRole::Required:
                    view.identifiedBy.emplace_back(row);
                    break;
                case annotation::SignatureRole::Forbidden:
                    view.mustNotShow.emplace_back(row);
                    break;
                }
            }
            if (reference.exercised.interact.has_value())
            {
                view.regions.emplace_back(row);
            }
            if (reference.exercised.read.has_value())
            {
                view.infos.emplace_back(row);
            }
        }
        return view;
    }

    auto PageView::all(AuthoringDraft const& draft) -> std::vector<PageView>
    {
        auto views = std::vector<PageView>{};
        views.reserve(draft.pages.size());
        for (auto const& page : draft.pages)
        {
            if (auto view = PageView::of(draft, page.id))
            {
                views.emplace_back(*std::move(view));
            }
        }
        return views;
    }
}
