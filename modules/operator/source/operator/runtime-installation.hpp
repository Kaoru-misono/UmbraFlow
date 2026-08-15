#pragma once

#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace uf::operator_runtime::detail
{
    // The generation of schema/umbraflow-annotation-workspace-v2.schema.json
    // this deployment principal reads, and the generation of the authoring
    // workspace database it accepts a release from. Both are the acceptor's
    // half of a compatibility statement, so a release names a number and this
    // side decides whether it understands what that number describes.
    //
    // They are generations rather than digests deliberately. The digests that
    // stood here made a cosmetic edit to the schema file, or a comment inside
    // the workspace DDL, refuse every release already published -- a cost paid
    // on every edit for a property no reader needed. tools/annotate/store.py
    // owns both numbers as ANNOTATION_WORKSPACE_FORMAT and SCHEMA_VERSION and
    // stamps them in tools/annotate/publication.py; bumping either is a
    // deliberate two-repository decision, not a side effect of editing bytes.
    inline constexpr auto k_annotationWorkspaceFormat = uint64{2U};
    inline constexpr auto k_workspaceSqliteRevision   = uint64{2U};

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
