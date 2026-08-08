#pragma once

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <span>

namespace uf::cli
{
    // Resolves the handle the operator named to the enumerated window carrying
    // it, and refuses one that cannot be captured. Capturable means visible and
    // not minimized: an anti-automation shield surrounds a real game window with
    // dozens of invisible decoys, and a minimized window composites no frames at
    // all.
    //
    // Each refusal names which of the three things went wrong, because the
    // operator's next move differs for each: a handle that no window carries has
    // to be re-read, an invisible one is a decoy, and a minimized one is the
    // right window in the wrong state. The result is an owning copy.
    [[nodiscard]]
    auto selectCandidate(
        std::span<TargetCandidate const> candidates,
        WindowHandle handle
    ) -> Result<TargetCandidate>;
}
