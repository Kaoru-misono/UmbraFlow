#include "targets.hpp"

#include <controller/discovery.hpp>
#include <controller/dpi.hpp>

#include <core/error/result.hpp>

#include <vector>

namespace uf::cli
{
    auto targetsProduct() -> Result<std::vector<TargetListing>>
    {
        // Before enumerating, exactly as bindTarget orders it. A DPI-unaware
        // process is told scaled client sizes, so a listing taken without this
        // would print geometry no capture session will ever see.
        UF_TRY(ensurePerMonitorAwareV2());
        UF_TRY_VALUE(candidates, enumerateCandidates());

        auto listings = std::vector<TargetListing>{};
        for (auto const& candidate : candidates)
        {
            // The invisible ones are the shield's decoys, dozens per real
            // window, carrying the real title plus a suffix. No operator can
            // mean one, and printing them would bury the window that matters.
            if (!candidate.isVisible())
            {
                continue;
            }

            auto const client = candidate.clientSize();
            listings.emplace_back(
                TargetListing{
                    .handle       = candidate.handle().value(),
                    .windowClass  = candidate.windowClass(),
                    .title        = candidate.title(),
                    .clientWidth  = client.width(),
                    .clientHeight = client.height(),
                    .dpi          = candidate.dpi().value(),
                    .isIconic     = candidate.isIconic(),
                }
            );
        }
        return listings;
    }
}
