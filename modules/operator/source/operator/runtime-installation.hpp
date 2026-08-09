#pragma once

#include <task/page-model-file.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace uf::operator_runtime::detail
{
    // The exact bytes of schema/umbraflow-annotation-workspace-v2.schema.json,
    // which every release manifest must name. It sits in the header so the
    // deployment check and the test fixtures share one spelling;
    // tests/test-runtime-surface.py pins it to the checked-in file so the two
    // cannot drift apart again.
    inline constexpr auto k_annotationWorkspaceSchemaHash = std::string_view{
        "a6fc31b5e0ee49f5368d66fae3f2abf38e0e58f57d799e3d2cd8da583f508a29"
    };

    struct PreparedRuntimeInstallation final
    {
        std::shared_ptr<task::RuntimeArtifactHandle const> artifact;
        ContentHash                                        releaseManifestHash;
        ContentHash                                        artifactRootHash;
        std::filesystem::path                              productionPath;
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
