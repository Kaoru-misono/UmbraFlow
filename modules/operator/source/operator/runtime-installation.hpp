#pragma once

#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <memory>
#include <string_view>

namespace uf::operator_runtime::detail
{
    // The one child of the production root that is not a content hash. It is
    // named here because the installer stages into it and reclamation sweeps
    // it, and a second spelling would let those two disagree.
    inline constexpr auto k_stagingDirectoryName = std::string_view{".staging"};

    // Holds the RuntimeArtifact directory an operator supplied against the root
    // hash that same operator stated, and returns the frozen handle for it. The
    // artifact's own manifest bytes are the only thing hashed, so the directory
    // needs no document beside it to be trustworthy.
    //
    // It is separated from publication so that the caller holds a verified
    // handle BEFORE anything is written under the production root, which is
    // what lets the Operator record a claim on that hash first.
    [[nodiscard]]
    auto readRuntimeArtifactSource(
        std::filesystem::path const& productionRoot,
        std::filesystem::path const& artifactDirectory,
        ContentHash const& artifactRootHash
    ) -> Result<task::RuntimeArtifactHandle>;

    // Materializes the verified artifact into the production root and reopens
    // it from there. The caller must already hold a claim on
    // source.rootHash(): this is the step that creates the directory
    // reclamation would otherwise be free to remove.
    [[nodiscard]]
    auto publishRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        task::RuntimeArtifactHandle const& source,
        std::string_view stagingToken
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>;

    [[nodiscard]]
    auto openProductionRuntimeArtifact(
        std::filesystem::path const& productionRoot,
        ContentHash const& artifactRootHash
    ) -> Result<std::shared_ptr<task::RuntimeArtifactHandle const>>;
}
