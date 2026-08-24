#pragma once

#include "effective-plan.hpp"
#include "manifest.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
#include <functional>
#include <limits>
#include <string_view>

namespace uf::operator_runtime
{
    // What OP:`AgentBudget`'s "unbounded" marker denotes once it is read.
    //
    // It is the largest value the ledger's own INTEGER column holds, so a
    // ceiling that does not bind is still a number the durable row carries,
    // still charged by the same UPDATE, and still refused by the same CHECK if
    // anything ever reached it. There is no second code path for an unbounded
    // session and no nullable column standing for one -- an absence in the row
    // would be the ledger declining to say what the Operator granted.
    inline constexpr auto k_unboundedBudget =
        static_cast<uint64>(std::numeric_limits<int64>::max());

    // What one session may spend before the Operator stops answering it. The
    // member names are OP:`AgentBudget`'s, because the Operator protocol schema
    // is the fixed side and a second spelling of one ceiling is how two of them
    // come to disagree.
    //
    // EVERY session declares one, whoever controls it. A script, a human and an
    // online agent all spend the operator's machine, and a budget that only one
    // kind carried would be the framework deciding that the other two need no
    // ceiling. What varies between them is the numbers the operator wrote --
    // including k_unboundedBudget, which is a number the operator wrote.
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

    // The exact canonical bytes of a profile whose every ceiling is the
    // unbounded marker.
    //
    // It exists so that a session the framework itself starts on behalf of the
    // person at the terminal -- `umbra-flow observe`, `umbra-flow upgrade` --
    // pins a budget that SAYS what it grants. Nothing here is a default: these
    // bytes are hashed into the session manifest, so an unbounded grant is
    // attributable to the run that took it, exactly as a narrow one is. A
    // session whose budget somebody else chose reads that somebody's bytes
    // instead.
    inline constexpr auto k_unboundedAgentProfileJcs = std::string_view{
        R"({"maximum_elapsed_ms":"unbounded",)"
        R"("maximum_mutations":"unbounded",)"
        R"("maximum_observations":"unbounded",)"
        R"("maximum_risk_units":"unbounded",)"
        R"("maximum_tool_calls":"unbounded"})"
    };

    // Trusted deployment callback. It parses the exact AgentProfile bytes the
    // session manifest pins and returns the ceilings they state. It is never
    // passed to plugin code or published in a business VM.
    using AgentProfileValidator = std::function<
        Result<AgentBudget>(std::string_view exactProfileJcs)
    >;

    // The ceilings of one session, bound to the exact bytes that session's
    // SessionManifest names in agent_profile_hash. Every session has one; see
    // AgentBudget for why the controller kind does not vary it.
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
