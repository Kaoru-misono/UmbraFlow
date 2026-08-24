#pragma once

#include <core/error/result.hpp>

#include <filesystem>
#include <string_view>

namespace uf::cli
{
    // Where a project keeps the RuntimeArtifact its own declaration names. It
    // is the path `project init` writes the genesis artifact to and the path
    // the scaffolded umbraflow-project.json declares in runtime_artifact, so a
    // directory this skeleton laid out and a directory the kit scaffolded agree
    // without either reading the other.
    inline constexpr auto k_projectRuntimeArtifactPath = std::string_view{
        "runtime/artifact"
    };

    // Lays out the directories an annotation session writes into, for a project
    // directory that does not have them yet.
    //
    // This is the CLI's job because `task::ProjectFileStore` refuses a name whose
    // parent directory does not exist: canonicalizing a parent that is really there
    // is HOW a write proves it did not leave the project, so a store that created
    // the parent would prove confinement against a directory it had just invented.
    // The model above it reaches the disk only through that store, and "create a
    // directory" is not a verb the script layer has. What is left is this
    // composition root, which opens the project before any of that exists.
    //
    // It creates only what is missing and never removes or replaces anything, so
    // running it over an established project is a no-op. A path that already exists
    // as something other than a directory is reported rather than overwritten:
    // guessing which one the caller meant is not this function's decision.
    // It also writes the genesis RuntimeArtifact when the project has none, so
    // a directory nobody has annotated yet still HAS a model: the one that
    // declares nothing. That is a project initialisation and not a default --
    // every project in the universe starts from the same H_genesis, and a
    // directory with no artifact at all could not open a session at all.
    [[nodiscard]]
    auto ensureProjectSkeleton(std::filesystem::path const& projectRoot) -> Status;
}
