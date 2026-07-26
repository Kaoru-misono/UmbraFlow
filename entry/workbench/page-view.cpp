#include "page-view.hpp"

#include "authoring-edit.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <algorithm>
#include <optional>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        [[nodiscard]]
        auto anchorRow(EditableRecognizer const& recognizer) -> PageView::AnchorRow
        {
            return PageView::AnchorRow{
                .id           = recognizer.id,
                .name         = recognizer.name,
                .templateRect = recognizer.templateRect,
                .searchRoi    = recognizer.searchRoi,
                .shared       = recognizer.shared,
            };
        }

        [[nodiscard]]
        auto regionRow(EditableRecognizer const& recognizer) -> PageView::RegionRow
        {
            return PageView::RegionRow{
                .id                  = recognizer.id,
                .name                = recognizer.name,
                .templateRect        = recognizer.templateRect,
                .searchRoiOnThisPage = recognizer.searchRoi,
                .clickOffset         = recognizer.defaultClick,
                .shared              = recognizer.shared,
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

        auto const rowFor = [&draft](annotation::RecognizerId member)
            -> std::optional<PageView::AnchorRow>
        {
            auto const found = std::ranges::find(
                draft.recognizers,
                member,
                &EditableRecognizer::id
            );
            if (found == draft.recognizers.end())
            {
                return std::nullopt;
            }
            return anchorRow(*found);
        };

        auto identifiedBy = std::vector<PageView::AnchorRow>{};
        identifiedBy.reserve(page->required.size());
        for (auto const& member : page->required)
        {
            if (auto row = rowFor(member))
            {
                identifiedBy.emplace_back(*std::move(row));
            }
        }

        auto mustNotShow = std::vector<PageView::AnchorRow>{};
        mustNotShow.reserve(page->forbidden.size());
        for (auto const& member : page->forbidden)
        {
            if (auto row = rowFor(member))
            {
                mustNotShow.emplace_back(*std::move(row));
            }
        }

        auto regions = std::vector<PageView::RegionRow>{};
        for (auto const& recognizer : draft.recognizers)
        {
            auto const placedHere = (
                recognizer.annotationType
                    == annotation::AnnotationType::ActionTarget
                && std::ranges::contains(recognizer.allowedPageIds, id)
            );
            if (placedHere)
            {
                regions.emplace_back(regionRow(recognizer));
            }
        }

        return PageView{
            .id            = page->id,
            .name          = page->name,
            .claimedScreen = claimedScreenFor(draft, id),
            .identifiedBy  = std::move(identifiedBy),
            .mustNotShow   = std::move(mustNotShow),
            .regions       = std::move(regions),
        };
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
