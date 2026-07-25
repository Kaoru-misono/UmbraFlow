#include "candidate-selection.hpp"

#include <domain/error.hpp>

#include <format>
#include <string>
#include <vector>

namespace uf::cli
{
    auto selectCandidate(
        std::span<TargetCandidate const> candidates,
        std::string_view selector
    ) -> Result<TargetCandidate>
    {
        auto matches = std::vector<TargetCandidate const*>{};
        for (auto const& candidate : candidates)
        {
            if (
                candidate.isVisible()
                && !candidate.isIconic()
                && candidate.title().find(selector) != std::string::npos
            )
            {
                matches.emplace_back(&candidate);
            }
        }

        if (matches.empty())
        {
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "no visible window title contains \"{}\"",
                    selector
                )
            );
        }
        if (matches.size() > 1U)
        {
            auto titles = std::string{};
            for (auto const* p_match : matches)
            {
                if (!titles.empty())
                {
                    titles += ", ";
                }
                titles += '"';
                titles += p_match->title();
                titles += '"';
            }
            return fail(
                AutomationErrorKind::TargetUnavailable,
                std::format(
                    "selector \"{}\" matches {} visible windows; refine it: {}",
                    selector,
                    matches.size(),
                    titles
                )
            );
        }

        return *matches.front();
    }
}
