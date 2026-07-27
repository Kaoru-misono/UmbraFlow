#pragma once

#include <annotation/authoring-document.hpp>
#include <annotation/catalog.hpp>

#include <core/types/integer.hpp>

#include <vector>

namespace uf::workbench
{
    // Which of the project tree's screen groups a captured screen belongs to,
    // derived purely from its regression case. The tree draws three pageless
    // buckets plus the per-page regression lists; this is the classification a
    // screen row is placed by.
    //
    //   NeedsClassification -- no regression case at all: the true to-do, the
    //     only bucket the tree presents as work outstanding.
    //   ExpectedUnknown     -- a case recorded as resolving to none of the pages.
    //   ExpectedAmbiguous   -- a case recorded as deliberately ambiguous.
    //   Resolved            -- a case naming a page; the screen belongs under that
    //     page's regression list, never in a top-level bucket.
    enum class ScreenBucket : uint8
    {
        NeedsClassification,
        ExpectedUnknown,
        ExpectedAmbiguous,
        Resolved,
    };

    // The bucket a single screen falls in. Pure over the document: a screen with
    // no case is unclassified work; one with a resolved case is owned by its
    // page.
    [[nodiscard]]
    auto screenBucketOf(
        annotation::AuthoringDocument const& document,
        annotation::SourceId sourceId
    ) -> ScreenBucket;

    // Every screen in one bucket, in the document's source order. For a Resolved
    // request this is every screen owned by some page; regressionScreensForPage
    // narrows that to one page.
    [[nodiscard]]
    auto screensInBucket(
        annotation::AuthoringDocument const& document,
        ScreenBucket bucket
    ) -> std::vector<annotation::SourceId>;

    // Every screen whose resolved regression case names this page, in the
    // document's source order. A page relates to screens through per-source cases
    // and several may resolve to the same page, so this is a list rather than the
    // single first-match claimedScreen shortcut.
    [[nodiscard]]
    auto regressionScreensForPage(
        annotation::AuthoringDocument const& document,
        annotation::PageId pageId
    ) -> std::vector<annotation::SourceId>;
}
