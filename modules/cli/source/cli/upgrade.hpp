#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <string>

namespace uf::cli
{
    // What one upgrade published and pinned, restated in the shape this
    // binary prints.
    //
    // The generation and the session id are the Operator's answers, not the
    // caller's claims: the installed generation is re-read from the root's
    // active pin after the upgrade, and the session id is the row the ledger
    // pinned. The hashes are stated as hex strings for the reason
    // OpenedProject restates a load as strings: uf::operator_runtime is a
    // private dependency of this module, so nothing a caller of this header
    // reads obliges it to link the authority that answered.
    struct UpgradedRuntime final
    {
        std::filesystem::path project{};
        std::filesystem::path runtime{};
        std::filesystem::path handoff{};

        uint64      installedGeneration{};
        std::string artifactRootHash{};
        std::string sessionId{};

        auto operator==(UpgradedRuntime const&) const -> bool = default;
    };

    // Publishes the release handoff and pins its session through the one
    // production door.
    [[nodiscard]]
    auto upgradeProduct(UpgradeArgs const& args) -> Result<UpgradedRuntime>;

    // Separate from the publication so the shape an operator reads is
    // testable without a root on disk, as formatOpenedProject is.
    [[nodiscard]]
    auto formatUpgradedRuntime(UpgradedRuntime const& upgraded) -> std::string;
}
