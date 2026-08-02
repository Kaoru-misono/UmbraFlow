#pragma once

#include <core/error/result.hpp>

#include <filesystem>

namespace uf::cli
{
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
    [[nodiscard]]
    auto ensureProjectSkeleton(std::filesystem::path const& projectRoot) -> Status;
}
