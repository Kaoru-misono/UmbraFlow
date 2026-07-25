#pragma once

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <span>
#include <string_view>

namespace uf::cli
{
    // Picks the single capturable candidate whose title contains the selector
    // substring. A capturable candidate is visible and not minimized: an
    // anti-automation shield surrounds a real game window with dozens of
    // invisible decoy windows carrying near-identical titles, and a minimized
    // window cannot be captured, so neither participates in selection. Zero or
    // multiple matches fail TargetUnavailable with a message the caller can act
    // on, so the flow never guesses which window the operator meant. The result
    // is an owning copy; it does not alias the input span.
    [[nodiscard]]
    auto selectCandidate(
        std::span<TargetCandidate const> candidates,
        std::string_view selector
    ) -> Result<TargetCandidate>;
}
