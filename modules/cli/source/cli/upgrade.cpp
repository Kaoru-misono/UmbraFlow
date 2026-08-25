#include "upgrade.hpp"

#include <service/product-lifecycle.hpp>

#include <core/error/result.hpp>

#include <format>
#include <string>
#include <utility>

namespace uf::cli
{
    auto upgradeProduct(UpgradeArgs const& args) -> Result<UpgradedRuntime>
    {
        UF_TRY_VALUE(
            upgraded,
            service::upgradeRuntimeArtifactAndPinSession(
                service::RuntimeUpgradeStart{
                    .projectDirectory       = args.project,
                    .runtimeDirectory       = args.runtime,
                    .artifactDirectory      = args.artifact,
                    .artifactRootHash       = args.artifactRootHash,
                    .controllerCapabilities = args.capabilities,
                }
            )
        );
        return UpgradedRuntime{
            .project             = args.project,
            .runtime             = args.runtime,
            .artifact            = args.artifact,
            .installedGeneration = upgraded.installedGeneration,
            .artifactRootHash    = upgraded.artifactRootHash.hex(),
            .sessionId           = std::move(upgraded.sessionId),
        };
    }

    auto formatUpgradedRuntime(UpgradedRuntime const& upgraded) -> std::string
    {
        return std::format(
            "{:<24}{}\n"
            "{:<24}{}\n"
            "{:<24}{}\n"
            "{:<24}{}\n"
            "{:<24}{}\n"
            "{:<24}{}\n",
            "upgraded project",
            upgraded.project.string(),
            "runtime root",
            upgraded.runtime.string(),
            "source artifact",
            upgraded.artifact.string(),
            "artifact root hash",
            upgraded.artifactRootHash,
            "installed generation",
            upgraded.installedGeneration,
            "upgrade session",
            upgraded.sessionId
        );
    }
}
