#pragma once

#include "authoring-edit.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>
#include <annotation/resource.hpp>

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
        // The page-local key for a member of a page: the element id, which is
        // what an authoring edit addresses and what one reference per (page,
        // element) makes unambiguous.
        using MemberId = annotation::ElementId;

        // One element's authored data as it sits on one page. Ids and authored
        // values only: margins and live scores are owned by the last model check
        // and merged in by the panels at draw time, never embedded here.
        //
        // One row type covers every capability, because a capability set means
        // one element can be several of them at once -- the same patch of pixels
        // naming its page and being clickable is the case the whole model change
        // exists for -- so an element appears in every group its reference
        // exercises, as the same row.
        struct MemberRow final
        {
            MemberId    id;
            std::string name{};

            // The sole appearance's rectangle. Absent for an element located by
            // the page rather than by pixels of its own, which is a legal state
            // and the one a readable cell is in.
            std::optional<PixelRect> templateRect{};

            // The region searched for it here: this page's refinement when it
            // made one, the element's own otherwise.
            PixelRect searchRoiOnThisPage;

            std::optional<EditableTemplateOffset> clickOffset{};

            // Whether these pixels are this page's own or borrowed from the page
            // that owns them. This is the editing guard rail the old reuse flag
            // could only approximate: it says where the element's home is, so an
            // edit that would move it everywhere can be named as such.
            annotation::Holding holding{annotation::Holding::Owned};
        };

        annotation::PageId                  id;
        std::string                         name{};
        std::optional<annotation::SourceId> claimedScreen{};

        // The four ways a page's reference exercises an element: as evidence for
        // the page, as evidence against it, as something to click, as something
        // to read. One element can occupy several of these at once.
        std::vector<MemberRow> identifiedBy{};
        std::vector<MemberRow> mustNotShow{};
        std::vector<MemberRow> regions{};
        std::vector<MemberRow> infos{};

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
