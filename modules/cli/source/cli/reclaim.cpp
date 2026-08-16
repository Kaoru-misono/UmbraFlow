#include "reclaim.hpp"

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>

#include <format>
#include <string>
#include <utility>

namespace uf::cli
{
    auto reclaimProduct(ReclaimArgs const& args) -> Result<ReclaimedRuntime>
    {
        UF_TRY_VALUE(
            reclaimed,
            service::reclaimRuntimeArtifacts(args.runtime)
        );
        return ReclaimedRuntime{
            .runtime             = args.runtime,
            .artifactDirectories = reclaimed.artifactDirectories,
            .stagingDirectories  = reclaimed.stagingDirectories,
        };
    }

    auto formatReclaimedRuntime(ReclaimedRuntime const& reclaimed) -> std::string
    {
        // The counts are printed whether or not they are zero. A sweep that
        // found nothing is the ordinary outcome, and a verb that stayed silent
        // about it would leave a reader unable to tell it from a verb that did
        // not run.
        return std::format(
            "{:<22}{}\n"
            "  {:<24}{}\n"
            "  {:<24}{}\n",
            "reclaimed runtime",
            reclaimed.runtime.string(),
            "artifact directories",
            reclaimed.artifactDirectories,
            "staging directories",
            reclaimed.stagingDirectories
        );
    }
}
