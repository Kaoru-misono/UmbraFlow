#include "project-tree.hpp"

#include <annotation/authoring-document.hpp>
#include <annotation/resource.hpp>

#include <algorithm>
#include <variant>
#include <vector>

namespace uf::workbench
{
    namespace
    {
        // The regression case recorded for one screen, or nullptr when the screen
        // carries none. A screen holds at most one case, so the first match is the
        // only match.
        [[nodiscard]]
        auto caseFor(
            annotation::AuthoringDocument const& document,
            annotation::SourceId sourceId
        ) -> annotation::RegressionCase const*
        {
            for (auto const& regression : document.regressions())
            {
                if (regression.sourceId() == sourceId)
                {
                    return &regression;
                }
            }
            return nullptr;
        }
    }

    auto screenBucketOf(
        annotation::AuthoringDocument const& document,
        annotation::SourceId sourceId
    ) -> ScreenBucket
    {
        auto const* p_case = caseFor(document, sourceId);
        if (p_case == nullptr)
        {
            return ScreenBucket::NeedsClassification;
        }
        auto const& expectation = p_case->expectation();
        if (std::holds_alternative<annotation::ResolvedRegression>(expectation))
        {
            return ScreenBucket::Resolved;
        }
        if (std::holds_alternative<annotation::AmbiguousRegression>(expectation))
        {
            return ScreenBucket::ExpectedAmbiguous;
        }
        return ScreenBucket::ExpectedUnknown;
    }

    auto screensInBucket(
        annotation::AuthoringDocument const& document,
        ScreenBucket bucket
    ) -> std::vector<annotation::SourceId>
    {
        auto screens = std::vector<annotation::SourceId>{};
        for (auto const& source : document.sources())
        {
            if (screenBucketOf(document, source.id()) == bucket)
            {
                screens.emplace_back(source.id());
            }
        }
        return screens;
    }

    auto regressionScreensForPage(
        annotation::AuthoringDocument const& document,
        annotation::PageId pageId
    ) -> std::vector<annotation::SourceId>
    {
        auto screens = std::vector<annotation::SourceId>{};
        for (auto const& source : document.sources())
        {
            auto const* p_case = caseFor(document, source.id());
            if (p_case == nullptr)
            {
                continue;
            }
            auto const* p_resolved = std::get_if<annotation::ResolvedRegression>(
                &p_case->expectation()
            );
            if (p_resolved != nullptr && p_resolved->pageId == pageId)
            {
                screens.emplace_back(source.id());
            }
        }
        return screens;
    }
}
