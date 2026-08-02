#pragma once

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <span>
#include <string_view>

namespace uf::cli
{
    // Picks the single capturable candidate whose title contains the selector
    // substring. Capturable means visible and not minimized: an anti-automation
    // shield surrounds a real game window with dozens of invisible decoy windows
    // carrying near-identical titles, and a minimized window cannot be captured.
    // Zero or multiple matches fail TargetUnavailable rather than guessing which
    // window the operator meant. The result is an owning copy.
    [[nodiscard]]
    auto selectCandidate(
        std::span<TargetCandidate const> candidates,
        std::string_view selector
    ) -> Result<TargetCandidate>;
}
