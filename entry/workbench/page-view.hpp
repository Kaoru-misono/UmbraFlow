#pragma once

#include "authoring-edit.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <core/types/integer.hpp>

#include <domain/space.hpp>

#include <optional>
#include <string>
#include <vector>

namespace uf::workbench
{
    // A per-frame value snapshot of one page: the object the panels iterate to
    // draw it. Authored data and ids only -- no margins, no verdicts, no live
    // scores. Those stay owned once by the last model check and are merged at
    // draw time through the existing id-keyed lookups, so a completed check does
    // not force a view rebuild and no second owner of "the score for X" exists.
    struct PageView final
    {
        // The page-local key for a member of a page. In this phase it is the
        // recognizer id: the v1 model has no element identity, and a region
        // shared onto page P is its own recognizer there, so keying a member by
        // recognizer is what makes an edit to "this page's copy" unambiguous.
        // The name carries the role rather than the current representation:
        // phase 3 rebases it onto the placement key (an element on this page)
        // without changing this API.
        using MemberId = annotation::RecognizerId;

        // One anchor's authored data, for drawing a page's signature. Ids and
        // authored values only: margins and live scores are owned by the last
        // model check and merged in by the panels at draw time, never embedded
        // here.
        struct AnchorRow final
        {
            MemberId    id;
            std::string name{};
            PixelRect   templateRect;
            PixelRect   searchRoi;
            bool        shared{};
        };

        // One placeable element's authored data as it sits on a page. The search
        // ROI is this page's own: under v2 it is the placement's per-page region.
        // Both interactive and info elements use this row; an info element carries
        // no click offset.
        struct RegionRow final
        {
            MemberId                              id;
            std::string                           name{};
            PixelRect                             templateRect;
            PixelRect                             searchRoiOnThisPage;
            std::optional<EditableTemplateOffset> clickOffset{};
            bool                                  shared{};
        };

        annotation::PageId                  id;
        std::string                         name{};
        std::optional<annotation::SourceId> claimedScreen{};

        std::vector<AnchorRow> identifiedBy{};
        std::vector<AnchorRow> mustNotShow{};
        std::vector<RegionRow> regions{};

        // Info elements placed on this page. Kept apart from interactive regions
        // so the pages panel can label them, but drawn the same way -- so nothing
        // placeable is ever invisible, which is what left a retyped Info region
        // reachable only through undo.
        std::vector<RegionRow> infos{};

        // Builds the snapshot for one page of a draft. A page absent from the
        // draft yields nothing, because there is no authored data to draw.
        [[nodiscard]]
        static auto of(
            AuthoringDraft const& draft,
            annotation::PageId id
        ) -> std::optional<PageView>;

        // Builds a snapshot per page, in the draft's page order, so the pages
        // panel draws every page reflectively from one pass.
        [[nodiscard]]
        static auto all(AuthoringDraft const& draft) -> std::vector<PageView>;
    };
}
