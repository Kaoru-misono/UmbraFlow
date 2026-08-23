#pragma once

#include "effective-plan.hpp"
#include "manifest.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <functional>
#include <string_view>

namespace uf::operator_runtime
{
    // What one online Agent binding may spend before the Operator stops
    // answering it. The member names are OP:`AgentBudget`'s, because the
    // Operator protocol schema is the fixed side and a second spelling of one
    // ceiling is how two of them come to disagree.
    //
    // There is deliberately no no-progress millisecond member. The whole
    // binding already has maximumElapsedMillis, and a second elapsed axis would
    // need another clock marker and another ceiling without distinguishing a
    // state that one leaves open.
    struct AgentBudget final
    {
        uint64 maximumToolCalls{};
        uint64 maximumMutations{};
        uint64 maximumObservations{};
        uint64 maximumElapsedMillis{};
        uint64 maximumRiskUnits{};
    };

    // What one binding has left. Read back rather than returned from each
    // accept, so that there is one reader of the stored counters and a test
    // that asserts a decrement happened is asserting on the database rather
    // than on a value the same call computed.
    struct AgentBudgetRemaining final
    {
        uint64 toolCalls{};
        uint64 mutations{};
        uint64 observations{};
        uint64 riskUnits{};
        uint64 elapsedMillisRemaining{};
        uint64 consecutiveNoProgressSteps{};
    };

    // TODO(cpp-debt): riskUnits has no caller and k_agentNoProgressCeiling has
    // no enforcement site. remaining_risk_units was never charged -- that
    // predates the Operation cut -- but the no-progress ceiling was enforced
    // inside submitCommand, which the cut deleted, so today an Agent asking the
    // same thing of the same world is stopped by nothing. Both ceilings, their
    // agent_budgets columns and the budget_snapshot members that report them
    // stay declared rather than being deleted quietly, because deleting them
    // means rewriting recorded budget_snapshot audit JSON under a registered
    // migration and re-enforcing them means choosing what a Tool call's state
    // fingerprint is. Neither is this change's to decide; what is this
    // change's is to say plainly that the ceilings below are declared and
    // unenforced.
    [[nodiscard]] auto riskUnits(Risk risk) noexcept -> uint64;

    inline constexpr auto k_agentNoProgressCeiling = uint64{3};

    // Trusted deployment callback. It parses the exact AgentProfile bytes the
    // session manifest pins and returns the ceilings they state. It is never
    // passed to plugin code or published in a business VM.
    using AgentProfileValidator = std::function<
        Result<AgentBudget>(std::string_view exactProfileJcs)
    >;

    // The ceilings of one Agent session, bound to the exact bytes one
    // SessionManifest's agent_profile_hash names.
    //
    // The bytes are required rather than merely referenced, for
    // ProjectToolCatalogSchemaOwner's reason: without them a caller states a
    // budget that no manifest has to agree with, and the budget becomes a
    // number the controller chose. With them, changing a ceiling changes
    // agent_profile_hash, which changes session_manifest_hash, which changes
    // every decision_basis_hash the session goes on to compose -- so a
    // permissive budget is attributable rather than deniable.
    class AgentProfile final
    {
        AgentBudget m_budget;
        ContentHash m_sessionManifestHash;

        AgentProfile(AgentBudget budget, ContentHash sessionManifestHash);

    public:
        [[nodiscard]]
        static auto verifyExact(
            SessionManifest const& manifest,
            std::filesystem::path const& profilePath,
            std::string_view exactProfileBytes,
            AgentProfileValidator const& validate
        ) -> Result<AgentProfile>;

        [[nodiscard]] auto budget() const noexcept -> AgentBudget;
        [[nodiscard]] auto sessionManifestHash() const -> ContentHash;
    };
}
