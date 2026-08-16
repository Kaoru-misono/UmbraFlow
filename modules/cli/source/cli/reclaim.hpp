#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <filesystem>
#include <string>

namespace uf::cli
{
    // What one sweep removed, restated as counts.
    //
    // Two counts and not one, because the two kinds fail differently. An
    // artifact directory is content addressed and was published; a staging tree
    // is scratch an interrupted publication left. A run that reports staging
    // trees every time is a publisher that keeps dying, and a single total
    // would hide that.
    //
    // The Operator's own result type is not carried here for the reason
    // OpenedProject restates a load as strings: uf::operator_runtime is a
    // private dependency of this module, so nothing a caller of this header
    // reads obliges it to link the authority that answered.
    struct ReclaimedRuntime final
    {
        std::filesystem::path runtime{};

        uint64 artifactDirectories{};
        uint64 stagingDirectories{};

        auto operator==(ReclaimedRuntime const&) const -> bool = default;
    };

    // Opens the Operator root at args.runtime through the one production door
    // and runs the reclamation pass over it.
    //
    // The root is opened and closed by this call. That is what the Operator's
    // single-owner claim requires: the pass may not run beside a session, and
    // this verb finding the root already held is the refusal that enforces it
    // rather than a race it has to reason about.
    [[nodiscard]]
    auto reclaimProduct(ReclaimArgs const& args) -> Result<ReclaimedRuntime>;

    // Separate from the sweep so the shape an operator reads is testable
    // without a root on disk, as formatOpenedProject is.
    [[nodiscard]]
    auto formatReclaimedRuntime(ReclaimedRuntime const& reclaimed) -> std::string;
}
