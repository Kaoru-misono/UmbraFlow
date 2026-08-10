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

    // The one child of the production root that is not a content hash. It is
    // named here because the installer stages into it and reclamation sweeps
    // it, and a second spelling would let those two disagree.
    inline constexpr auto k_stagingDirectoryName = std::string_view{".staging"};

    // What a release handoff turns out to contain, once its manifest matches
    // the trusted deployment metadata and its RuntimeArtifact verifies. It is
    // separated from publication so that the caller learns the content hash
    // BEFORE anything is written under the production root, which is what lets
    // the Operator record a claim on that hash first.
    struct VerifiedRuntimeRelease final
    {
        task::RuntimeArtifactHandle handoffArtifact;
        ContentHash                 releaseManifestHash;
        ContentHash                 artifactRootHash;
    };

    [[nodiscard]]
    auto readRuntimeRelease(
        std::filesystem::path const& productionRoot,
        std::filesystem::path const& handoffRoot,
        ContentHash const& expectedReleaseManifestHash
    ) -> Result<VerifiedRuntimeRelease>;

    // Materializes the verified artifact into the production root and reopens
    // it from there. The caller must already hold a claim on
    // release.artifactRootHash: this is the step that creates the directory
    // reclamation would otherwise be free to remove.
    [[nodiscard]]
    auto publishRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        VerifiedRuntimeRelease const& release,
        std::string_view stagingToken
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>;

    [[nodiscard]]
    auto openProductionRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        ContentHash const& artifactRootHash
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>;
}
