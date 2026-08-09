#pragma once

#include <task/page-model-file.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace uf::operator_runtime::detail
{
    struct PreparedRuntimeInstallation final
    {
        std::shared_ptr<task::RuntimeArtifactHandle const> artifact;
        ContentHash releaseManifestHash;
        ContentHash artifactRootHash;
        std::filesystem::path productionPath;
    };

    [[nodiscard]]
    auto prepareRuntimeInstallation(
        std::filesystem::path const& productionRoot,
        std::filesystem::path const& handoffRoot,
        ContentHash const& expectedReleaseManifestHash,
        std::string_view stagingToken
    ) -> Result<PreparedRuntimeInstallation>;

    [[nodiscard]]
    auto openProductionRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        ContentHash const& artifactRootHash
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>;
}
