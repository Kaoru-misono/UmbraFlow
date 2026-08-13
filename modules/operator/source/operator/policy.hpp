#pragma once

#include "manifest.hpp"
#include "tool-descriptor.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    // What one policy rule concludes about one declared effect.
    enum class PolicyDecision : uint8
    {
        Allow,
        Deny,
        RequireApproval,
    };

    [[nodiscard]]
    auto policyDecisionWireName(PolicyDecision decision) noexcept
        -> std::string_view;

    // Which effects one rule speaks about. An empty list does not constrain its
    // dimension, which is why the artifact schema requires at least one of the
    // first two to be non-empty: a rule constraining neither would speak about
    // every effect of every tool.
    struct PolicyEffectSelector final
    {
        std::vector<std::string> toolNames{};
        std::vector<std::string> effectTypes{};
        std::vector<std::string> scopeKinds{};
    };

    // One rule of the artifact, in the Operator's terms.
    struct PolicyRule final
    {
        std::string ruleId{};

        PolicyEffectSelector selector{};

        // The capabilities a controller must hold for this rule to speak about
        // its command at all. A controller holding fewer does not fall through
        // to the rule's decision; the rule simply does not match, and the next
        // one -- or the artifact's default -- decides.
        std::vector<std::string> requiredControllerCapabilities{};

        // The capability an approver must present. Empty for every decision but
        // RequireApproval, which the artifact schema enforces both ways.
        std::string approverCapability{};

        uint64         priority{};
        Risk           maximumRisk{Risk::ReadOnly};
        PolicyDecision decision{PolicyDecision::Deny};
    };

    // What the Operator reads out of a PolicyArtifact. Like
    // ProjectRegistrationClaims this is not a construction spec: no caller can
    // hand one to the artifact and mint a verified policy from it.
    //
    // default_decision and unknown_effect_decision are absent because the
    // schema fixes both at `deny`. Carrying them would be carrying a choice
    // nobody has.
    struct PolicyArtifactClaims final
    {
        std::string             policyId{};
        std::string             policyVersion{};
        std::vector<PolicyRule> orderedRules{};
    };

    // What one policy evaluation is asked about. Every member is a call-scoped
    // borrow: evaluate builds no state from it, stores none of it, and it must
    // not be returned or stored by a caller either.
    struct PolicyRequest final
    {
        std::span<ProposedEffect const> effects{};

        // The capability set of the session presenting the command, read from
        // the sessions row. It is never a caller field: a controller that could
        // state its own capabilities could satisfy any rule it liked.
        std::span<std::string const> controllerCapabilities{};

        std::string_view toolName{};
    };

    // What the policy ruled. required_approvals is the set of approver
    // capabilities the matching rules named -- who may approve -- rather than a
    // flag derived from a risk level. A plan needing no approval carries an
    // empty set, and the two facts are therefore one value rather than two that
    // could disagree.
    struct PolicyVerdict final
    {
        std::vector<std::string> requiredApprovals{};
    };

    // A PolicyArtifact this session is pinned to. Its constructor is
    // unreachable except from verifyExact, which requires the exact bytes the
    // session manifest's policy_artifact_hash names: an authority that merely
    // named a hash would be a convention, and policy arriving as a hash a
    // caller supplied is the hole this type exists to close.
    class VerifiedPolicyArtifact final
    {
        PolicyArtifactClaims m_claims;
        std::string          m_canonicalJcs;
        ContentHash          m_hash;

        VerifiedPolicyArtifact(
            PolicyArtifactClaims claims,
            std::string canonicalJcs,
            ContentHash hash
        );

    public:
        [[nodiscard]]
        static auto verifyExact(
            SessionManifest const& sessionManifest,
            std::string_view exactPolicyArtifactBytes
        ) -> Result<VerifiedPolicyArtifact>;

        [[nodiscard]] auto hash() const -> ContentHash;

        [[nodiscard]]
        auto policyId() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        // Runs every declared effect past the ordered rules and returns what
        // they ruled, or refuses. A refusal is the artifact's `deny`: either a
        // rule denied the effect, or nothing allowed it and the artifact's
        // default did. Both are fail-closed and they are told apart by the
        // message, because "no rule speaks about this effect type at all" is
        // what an under-declaring or newly-extended project looks like.
        [[nodiscard]]
        auto evaluate(PolicyRequest const& request) const -> Result<PolicyVerdict>;
    };
}
