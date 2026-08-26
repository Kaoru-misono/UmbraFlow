#pragma once

#include "manifest.hpp"
#include "tool-descriptor.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <filesystem>
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

    // An Operator that has stated no policy gets this exact artifact: no rules,
    // no Privileged surface granted, with both defaults fixed to deny. It is
    // parameterized only by the exact Operator protocol schema it answers for.
    [[nodiscard]]
    auto denyAllPolicyArtifact(
        ContentHash const& operatorProtocolSchemaHash
    ) -> std::string;

    // Where the Operator writes the artifact, inside the production root it
    // already administers. The name is fixed rather than passed, for the same
    // reason the ledger and the RuntimeArtifact root are: a verb's caller does
    // not choose which policy judges it.
    inline constexpr auto k_operatorPolicyArtifactFileName = std::string_view{
        "policy-artifact.json"
    };

    // The PolicyArtifact this production root states, or the deny-all artifact
    // when the root states none.
    //
    // A PolicyArtifact governs who may act on this machine, so the party it
    // protects is the machine's owner and the supply path has to be that
    // party's. The production root is the one directory that party already
    // administers and every verb already opens, which is why the artifact is
    // read out of it rather than named by a flag on a command line the
    // requester writes.
    [[nodiscard]]
    auto operatorPolicyArtifact(
        std::filesystem::path const& runtimeDirectory,
        ContentHash const& operatorProtocolSchemaHash
    ) -> Result<std::string>;

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
    // ProjectGenerationClaims this is not a construction spec: no caller can
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

        // Every Tool this Operator permits to be reached on the Privileged
        // surface at the top of a run, by name. A Project classifies its own
        // Tools honestly and the framework keeps parsing that classification;
        // what the classification cannot do is authorise itself, so a name
        // absent from this list is refused at admission however the Project
        // labelled it. The list is stated even when empty, because an Operator
        // that grants no machine surface is saying so.
        std::vector<std::string> privilegedSurfaceTools{};
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

        // Whether this artifact names the Tool among the ones it permits on
        // the Privileged surface. It answers only the grant: whether the call
        // is a top-level one, and what a denied answer costs, are admission's.
        [[nodiscard]]
        auto grantsPrivilegedSurface(std::string_view toolName) const -> bool;

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
