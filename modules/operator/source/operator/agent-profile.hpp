#pragma once

#include "effective-plan.hpp"
#include "manifest.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string_view>

namespace uf::operator_runtime
{
    // What one online Agent binding may spend before the Operator stops
    // answering it. The member names are the frozen AgentBudget definition's,
    // because the schema is the fixed side and a second spelling of one
    // ceiling is how two of them come to disagree.
    //
    // There is deliberately no no-progress member: the frozen bundle's
    // AgentBudget has none, and adding one would move
    // operator_protocol_schema_hash and therefore every decision_basis_hash.
    // The no-progress ceiling is k_agentNoProgressCeiling below.
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

    // What one frozen plan costs the risk budget. It is a table over the risk
    // the Operator derived from the plugin's declared effects, so a plugin that
    // under-declares its own effects buys itself risk headroom -- the same
    // limit ToolMutability already lives with, and the reason the mutating
    // sub-count is sourced from the Tool Catalog instead.
    [[nodiscard]] auto riskUnits(Risk risk) noexcept -> uint64;

    // How many consecutive steps that changed neither the state fingerprint nor
    // the command fingerprint the Operator tolerates before it refuses.
    //
    // It is Operator-owned rather than an AgentBudget member for two reasons
    // that agree. The frozen bundle's AgentBudget has no such field, and adding
    // one is a bundle-root change. And a budget the agent's own profile
    // declares is not a budget for the one failure this axis exists to stop: an
    // Agent looping on an unchanging world would simply be deployed with a
    // larger ceiling.
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
            std::string_view exactProfileBytes,
            AgentProfileValidator const& validate
        ) -> Result<AgentProfile>;

        [[nodiscard]] auto budget() const noexcept -> AgentBudget;
        [[nodiscard]] auto sessionManifestHash() const -> ContentHash;
    };
}
