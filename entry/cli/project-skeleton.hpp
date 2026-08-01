#pragma once

#include <core/error/result.hpp>

#include <filesystem>

namespace uf::cli
{
    // Lays out the directories an annotation session writes into, for a project
    // directory that does not have them yet.
    //
    // WHY THIS IS THE CLI'S JOB AND NOT THE MODEL'S. `task::ProjectFileStore`
    // refuses a name whose parent directory does not exist, and that refusal is
    // load-bearing rather than incidental: canonicalizing a parent that is really
    // there is HOW a write proves it did not leave the project, so a store that
    // created the parent would be proving its confinement against a directory it
    // had just invented. The model above it cannot do the job either -- the
    // trusted framework reaches the disk only through that same store, and
    // "create a directory" is not a verb the script layer has or should get. What
    // is left is whoever opens the project before any of that exists, which is
    // this composition root.
    //
    // A session that meets the refusal instead pays it once per directory, and it
    // is the first thing an agent authoring a target from nothing runs into.
    //
    // It creates only what is missing and never removes or replaces anything, so
    // running it over an established project is a no-op. A path that already
    // exists as something other than a directory is reported rather than
    // overwritten: that is a project directory holding a file where a directory
    // belongs, and guessing which one the caller meant is not this function's
    // decision to make.
    [[nodiscard]]
    auto ensureProjectSkeleton(std::filesystem::path const& projectRoot) -> Status;
}
