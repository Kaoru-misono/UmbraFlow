#include "approve.hpp"

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>

#include <format>
#include <string>
#include <utility>

namespace uf::cli
{
    auto approveProduct(ApproveArgs const& args) -> Result<ApprovedRelease>
    {
        UF_TRY(service::approveReleaseCapabilities(
            args.runtime,
            operator_runtime::ReleaseCapabilityApproval{
                .artifactRootHash       = args.artifactRootHash,
                .controllerCapabilities = args.capabilities,
                .evidenceHash           = args.evidenceHash,
            }
        ));
        return ApprovedRelease{
            .runtime          = args.runtime,
            .artifactRootHash = args.artifactRootHash.hex(),
            .evidenceHash     = args.evidenceHash.hex(),
            .capabilities     = args.capabilities,
        };
    }

    auto formatApprovedRelease(ApprovedRelease const& approved) -> std::string
    {
        auto text = std::format(
            "{:<22}{}\n"
            "{:<22}{}\n"
            "{:<22}{}\n",
            "approved runtime",
            approved.runtime.string(),
            "artifact root hash",
            approved.artifactRootHash,
            "evidence hash",
            approved.evidenceHash
        );
        if (approved.capabilities.empty())
        {
            text += std::format("{:<22}{}\n", "capabilities", "(none)");
            return text;
        }
        for (auto const& capability : approved.capabilities)
        {
            text += std::format("{:<22}{}\n", "capabilities", capability);
        }
        return text;
    }
}
