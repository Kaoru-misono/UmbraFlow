#pragma once

#include "args.hpp"

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace uf::cli
{
    // What one approval recorded, restated in the shape this binary prints.
    //
    // The capabilities are the caller's stated set: the ledger stores the set
    // canonically, and the report's job is to say what was approved, not to
    // re-canonicalize a second spelling of the same rule. The two hashes are
    // hex strings for the reason OpenedProject restates a load as strings:
    // uf::operator_runtime is a private dependency of this module, so nothing
    // a caller of this header reads obliges it to link the authority that
    // answered.
    struct ApprovedRelease final
    {
        std::filesystem::path runtime{};

        std::string artifactRootHash{};
        std::string evidenceHash{};

        std::vector<std::string> capabilities{};

        auto operator==(ApprovedRelease const&) const -> bool = default;
    };

    // Records the approval through the one production door.
    [[nodiscard]]
    auto approveProduct(ApproveArgs const& args) -> Result<ApprovedRelease>;

    // Separate from the approval so the shape an operator reads is testable
    // without a root on disk, as formatOpenedProject is.
    [[nodiscard]]
    auto formatApprovedRelease(ApprovedRelease const& approved) -> std::string;
}
