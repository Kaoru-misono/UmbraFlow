#include "agent-profile.hpp"

#include <core/error/contracts.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <filesystem>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace uf::operator_runtime
{
    auto riskUnits(Risk risk) noexcept -> uint64
    {
        switch (risk)
        {
        case Risk::ReadOnly: return 0U;
        case Risk::Low: return 1U;
        case Risk::Medium: return 3U;
        case Risk::High: return 9U;
        case Risk::Critical: return 27U;
        }

        UF_UNREACHABLE_MSG("Unknown Risk value");
    }

    AgentProfile::AgentProfile(AgentBudget budget, ContentHash sessionManifestHash)
        : m_budget{budget}
        , m_sessionManifestHash{sessionManifestHash}
    {
    }

    auto AgentProfile::verifyExact(
        SessionManifest const& manifest,
        std::filesystem::path const& profilePath,
        std::string_view exactProfileBytes,
        AgentProfileValidator const& validate
    ) -> Result<AgentProfile>
    {
        if (!validate)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "AgentProfile requires a profile validator"
            );
        }
        UF_TRY_VALUE(
            profileHash,
            sha256(std::as_bytes(std::span{exactProfileBytes}))
        );
        if (profileHash != manifest.agentProfileHash())
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                std::format(
                    "AgentProfile file bytes do not match the manifest's "
                    "agent_profile_hash: \"{}\"",
                    profilePath.string()
                )
            );
        }
        UF_TRY_VALUE_CONTEXT(
            budget,
            validate(exactProfileBytes),
            "validating the AgentProfile against its own schema"
        );

        // A zero anywhere is a session that can do nothing at all, which is a
        // misconfiguration rather than a restriction: the schema's own minima
        // are 1 for tool calls, observations and elapsed time. Mutations and
        // risk units are allowed to be zero, because "may act but may not
        // change anything" is a real deployment.
        if (
            budget.maximumToolCalls == 0U
            || budget.maximumObservations == 0U
            || budget.maximumElapsedMillis == 0U
        )
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "AgentProfile must state a non-zero tool-call, observation and time budget"
            );
        }
        return AgentProfile{budget, manifest.hash()};
    }

    auto AgentProfile::budget() const noexcept -> AgentBudget
    {
        return m_budget;
    }

    auto AgentProfile::sessionManifestHash() const -> ContentHash
    {
        return m_sessionManifestHash;
    }
}
