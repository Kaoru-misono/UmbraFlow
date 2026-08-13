#include "policy.hpp"

#include "manifest.hpp"
#include "tool-descriptor.hpp"

#include <core/error/contracts.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <json/schema.hpp>
#include <json/value.hpp>

#include <schema/framework-schema-catalog.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_policyArtifactSchemaPath = std::string_view{
            "schema/umbraflow-policy-v1.schema.json"
        };

        struct DecisionName final
        {
            std::string_view wire{};
            PolicyDecision   decision{};
        };

        constexpr auto k_decisions = std::array{
            DecisionName{"allow", PolicyDecision::Allow},
            DecisionName{"deny", PolicyDecision::Deny},
            DecisionName{"require_approval", PolicyDecision::RequireApproval},
        };

        constexpr auto k_risks = std::array{
            Risk::ReadOnly,
            Risk::Low,
            Risk::Medium,
            Risk::High,
            Risk::Critical,
        };

        [[nodiscard]]
        auto refuse(std::string message) -> std::unexpected<Error>
        {
            return fail(AutomationErrorKind::InvalidResource, std::move(message));
        }

        // A json failure, restated in the Operator's vocabulary. The message is
        // carried whole because it names the clause that refused, which is the
        // entire diagnosis a red suite has.
        [[nodiscard]]
        auto adopt(Status outcome, std::string_view what) -> Status
        {
            if (outcome.has_value())
            {
                return ok();
            }
            return refuse(
                std::string{what} + ": " + std::string{outcome.error().message()}
            );
        }

        // A member the schema has already declared required, so its absence
        // would be a defect in this reader rather than in the document.
        [[nodiscard]]
        auto member(
            json::Value const& object,
            std::string_view name
        ) UF_LIFETIME_BOUND -> json::Value const&
        {
            auto const* const p_member = object.find(name);
            UF_CHECK(p_member != nullptr);
            return *p_member;
        }

        [[nodiscard]]
        auto stringList(json::Value const& array) -> std::vector<std::string>
        {
            auto values = std::vector<std::string>{};
            for (auto const& item : array.items())
            {
                values.emplace_back(item.string());
            }
            return values;
        }

        [[nodiscard]]
        auto readRule(json::Value const& rule) -> Result<PolicyRule>
        {
            auto const decision = std::ranges::find(
                k_decisions,
                member(rule, "decision").string(),
                &DecisionName::wire
            );
            auto const risk = std::ranges::find(
                k_risks,
                member(rule, "maximum_risk").string(),
                riskWireName
            );
            UF_CHECK(decision != k_decisions.end());
            UF_CHECK(risk != k_risks.end());

            auto const& approval    = member(rule, "approval");
            auto approverCapability = std::string{};
            if (approval.kind() == json::ValueKind::Object)
            {
                approverCapability = member(approval, "approver_capability").string();
            }

            auto const& selector = member(rule, "selector");
            auto const priority  = member(rule, "priority").number();
            if (priority > static_cast<double>(uint64{1} << 53U))
            {
                return refuse(
                    "a PolicyRule priority is outside the range a canonical "
                    "document can spell"
                );
            }
            return PolicyRule{
                .ruleId   = std::string{member(rule, "rule_id").string()},
                .selector = PolicyEffectSelector{
                    .toolNames   = stringList(member(selector, "tool_names")),
                    .effectTypes = stringList(member(selector, "effect_types")),
                    .scopeKinds  = stringList(member(selector, "scope_kinds")),
                },
                .requiredControllerCapabilities = stringList(
                    member(rule, "required_controller_capabilities")
                ),
                .approverCapability = std::move(approverCapability),
                .priority           = static_cast<uint64>(priority),
                .maximumRisk        = *risk,
                .decision           = decision->decision,
            };
        }

        [[nodiscard]]
        auto selects(
            std::span<std::string const> declared,
            std::string_view value
        ) -> bool
        {
            return declared.empty() || std::ranges::contains(declared, value);
        }

        [[nodiscard]]
        auto matches(
            PolicyRule const& rule,
            PolicyRequest const& request,
            ProposedEffect const& effect
        ) -> bool
        {
            return selects(rule.selector.toolNames, request.toolName)
                && selects(rule.selector.effectTypes, effect.namespacedType)
                && selects(rule.selector.scopeKinds, effect.scopeKind)
                && effect.risk <= rule.maximumRisk
                && std::ranges::all_of(
                       rule.requiredControllerCapabilities,
                       [&request](std::string const& capability)
                       {
                           return std::ranges::contains(
                               request.controllerCapabilities,
                               capability
                           );
                       }
                   );
        }

        // Whether any rule at all speaks about this effect type, ignoring
        // every other dimension it selects on. It separates "the policy refuses
        // this effect" from "the policy has never heard of it", which is what a
        // project that grew a new effect type looks like.
        [[nodiscard]]
        auto known(
            std::span<PolicyRule const> rules,
            std::string_view effectType
        ) -> bool
        {
            return std::ranges::any_of(
                rules,
                [effectType](PolicyRule const& rule)
                {
                    return selects(rule.selector.effectTypes, effectType);
                }
            );
        }
    }

    auto policyDecisionWireName(PolicyDecision decision) noexcept
        -> std::string_view
    {
        switch (decision)
        {
        case PolicyDecision::Allow: return "allow";
        case PolicyDecision::Deny: return "deny";
        case PolicyDecision::RequireApproval: return "require_approval";
        }

        UF_UNREACHABLE_MSG("Unknown PolicyDecision value");
    }

    auto denyAllPolicyArtifact(
        ContentHash const& operatorProtocolSchemaHash
    ) -> std::string
    {
        return std::format(
            "{{\"default_decision\":\"deny\","
            "\"operator_protocol_schema_hash\":\"{}\","
            "\"ordered_rules\":[],\"owned_by\":\"operator\","
            "\"policy_id\":\"operator-deny-all\",\"policy_version\":\"1\","
            "\"unknown_effect_decision\":\"deny\"}}",
            operatorProtocolSchemaHash.hex()
        );
    }

    VerifiedPolicyArtifact::VerifiedPolicyArtifact(
        PolicyArtifactClaims claims,
        std::string canonicalJcs,
        ContentHash hash
    )
        : m_claims{std::move(claims)}
        , m_canonicalJcs{std::move(canonicalJcs)}
        , m_hash{hash}
    {
    }

    auto VerifiedPolicyArtifact::verifyExact(
        SessionManifest const& sessionManifest,
        std::string_view exactPolicyArtifactBytes
    ) -> Result<VerifiedPolicyArtifact>
    {
        UF_TRY_VALUE(
            hash,
            sha256(std::as_bytes(std::span{exactPolicyArtifactBytes}))
        );
        if (hash != sessionManifest.policyArtifactHash())
        {
            return refuse(
                "PolicyArtifact bytes do not match the pinned session manifest"
            );
        }
        auto const published = framework_schema::findFrameworkSchema(
            k_policyArtifactSchemaPath
        );
        if (!published.has_value())
        {
            return refuse(
                "generated framework schema catalog is missing "
                + std::string{k_policyArtifactSchemaPath}
            );
        }
        UF_TRY_VALUE(
            schema,
            json::Schema::compile(json::Schema::Document{
                .label      = published->relativePath,
                .exactBytes = published->exactBytes,
            })
        );
        UF_TRY(adopt(
            json::requireExactCanonical(exactPolicyArtifactBytes),
            "the PolicyArtifact bytes"
        ));
        // requireExactCanonical parses in order to answer, so nothing that
        // reaches here is unparseable.
        UF_TRY_VALUE(document, json::parse(exactPolicyArtifactBytes));
        UF_TRY(adopt(schema.validate(document), "the PolicyArtifact"));

        UF_TRY_VALUE(
            protocolHash,
            ContentHash::parse(
                "sha256:"
                + std::string{
                    member(document, "operator_protocol_schema_hash").string()
                }
            )
        );
        if (protocolHash != sessionManifest.operatorProtocolSchemaHash())
        {
            return refuse(
                "PolicyArtifact answers for an operator protocol schema this "
                "session manifest does not pin"
            );
        }

        auto claims = PolicyArtifactClaims{
            .policyId      = std::string{member(document, "policy_id").string()},
            .policyVersion = std::string{
                member(document, "policy_version").string()
            },
            .orderedRules = {},
        };
        for (auto const& rule : member(document, "ordered_rules").items())
        {
            UF_TRY_VALUE(read, readRule(rule));
            claims.orderedRules.emplace_back(std::move(read));
        }
        for (
            auto index = std::size_t{1};
            index < claims.orderedRules.size();
            ++index
        )
        {
            // The array order decides which matching rule rules, so a priority
            // disagreeing with it would be a second authority over one
            // ordering. Stating both is allowed; disagreeing is not.
            if (
                claims.orderedRules[index - 1U].priority
                < claims.orderedRules[index].priority
            )
            {
                return refuse(
                    "PolicyArtifact ordered_rules are not in non-increasing "
                    "priority order, so the array order and the priorities "
                    "disagree about which rule decides"
                );
            }
        }
        auto ruleIds = std::vector<std::string>{};
        ruleIds.reserve(claims.orderedRules.size());
        for (auto const& rule : claims.orderedRules)
        {
            ruleIds.emplace_back(rule.ruleId);
        }
        std::ranges::sort(ruleIds);
        if (std::ranges::adjacent_find(ruleIds) != ruleIds.end())
        {
            return refuse("PolicyArtifact declares one rule_id twice");
        }
        return VerifiedPolicyArtifact{
            std::move(claims),
            std::string{exactPolicyArtifactBytes},
            hash,
        };
    }

    auto VerifiedPolicyArtifact::hash() const -> ContentHash
    {
        return m_hash;
    }

    auto VerifiedPolicyArtifact::policyId() const noexcept -> std::string const&
    {
        return m_claims.policyId;
    }

    auto VerifiedPolicyArtifact::canonicalJcs() const noexcept
        -> std::string const&
    {
        return m_canonicalJcs;
    }

    auto VerifiedPolicyArtifact::evaluate(
        PolicyRequest const& request
    ) const -> Result<PolicyVerdict>
    {
        auto approvals = std::vector<std::string>{};
        for (auto const& effect : request.effects)
        {
            auto const found = std::ranges::find_if(
                m_claims.orderedRules,
                [&request, &effect](PolicyRule const& rule)
                {
                    return matches(rule, request, effect);
                }
            );
            if (found == m_claims.orderedRules.end())
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    known(m_claims.orderedRules, effect.namespacedType)
                        ? "Policy " + m_claims.policyId
                            + " allows no rule for effect "
                            + effect.namespacedType
                            + ", and its default_decision is deny"
                        : "Policy " + m_claims.policyId
                            + " names no rule for effect type "
                            + effect.namespacedType
                            + ", and its unknown_effect_decision is deny"
                );
            }
            if (found->decision == PolicyDecision::Deny)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Policy rule " + found->ruleId + " denies effect "
                        + effect.namespacedType
                );
            }
            if (found->decision == PolicyDecision::RequireApproval)
            {
                approvals.emplace_back(found->approverCapability);
            }
        }
        std::ranges::sort(approvals);
        approvals.erase(std::ranges::unique(approvals).begin(), approvals.end());
        return PolicyVerdict{.requiredApprovals = std::move(approvals)};
    }
}
