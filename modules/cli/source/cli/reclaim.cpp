#include "reclaim.hpp"

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>

#include <format>
#include <string>
#include <utility>

namespace uf::cli
{
    auto reclaimProduct(ReclaimArgs const& args) -> Result<ReclaimedOperatorRoot>
    {
        UF_TRY_VALUE(
            reclaimed,
            service::reclaimOperatorStores(
                args.runtime,
                args.evidenceRetentionMillis
            )
        );
        return ReclaimedOperatorRoot{
            .runtime             = args.runtime,
            .artifactDirectories = reclaimed.runtime.artifactDirectories,
            .stagingDirectories  = reclaimed.runtime.stagingDirectories,
            .evidenceBlobs       = reclaimed.evidence.blobs,
            .evidenceStagings    = reclaimed.evidence.stagings,
        };
    }

    auto formatReclaimedOperatorRoot(
        ReclaimedOperatorRoot const& reclaimed
    ) -> std::string
    {
        // The counts are printed whether or not they are zero. A sweep that
        // found nothing is the ordinary outcome, and a verb that stayed silent
        // about it would leave a reader unable to tell it from a verb that did
        // not run.
        return std::format(
            "{:<22}{}\n"
            "  {:<24}{}\n"
            "  {:<24}{}\n"
            "  {:<24}{}\n"
            "  {:<24}{}\n",
            "reclaimed runtime",
            reclaimed.runtime.string(),
            "artifact directories",
            reclaimed.artifactDirectories,
            "staging directories",
            reclaimed.stagingDirectories,
            "evidence blobs",
            reclaimed.evidenceBlobs,
            "evidence stagings",
            reclaimed.evidenceStagings
        );
    }
}
