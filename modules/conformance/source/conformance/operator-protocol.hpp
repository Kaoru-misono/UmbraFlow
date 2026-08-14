#pragma once

#include "host-delivery-fixture.hpp"

#include <deployment/project-deployment.hpp>

#include <operator/effective-plan.hpp>

#include <core/error/result.hpp>

#include <domain/content-hash.hpp>
#include <domain/error.hpp>

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace uf::operator_runtime::conformance
{
    // The two capabilities the framework's own fixtures speak in: what a
    // session must hold for the policy below to speak about its commands at
    // all, and what an approver must present for an elevated one.
    inline constexpr auto k_operateCapability = std::string_view{"operate"};
    inline constexpr auto k_approveCapability = std::string_view{"approve"};

    // A PolicyArtifact for a run of the framework's own fixtures, in exact JCS.
    //
    // It is written here rather than in a project directory because policy is
    // Operator-owned: `owned_by` is `operator` in the artifact's own schema, and
    // a project that could supply the rules judging its own effects would be
    // deciding whether it may act. What a run does supply is which effect
    // types the rules speak about, because a rule selecting nothing would
    // speak about every effect of every tool.
    //
    // The three tiers it establishes are what the suites depend on: an effect at
    // or below medium risk is allowed, one at high risk needs an approver
    // holding k_approveCapability, and one at critical risk matches no rule and
    // falls to the artifact's default deny.
    [[nodiscard]]
    inline auto policyArtifactBytes(
        ContentHash const& operatorProtocolSchemaHash,
        std::span<std::string const> effectTypes
    ) -> std::string
    {
        auto selector = std::string{R"("selector":{"effect_types":[)"};
        auto first = true;
        for (auto const& type : effectTypes)
        {
            if (!first)
            {
                selector.push_back(',');
            }
            first = false;
            selector.push_back('"');
            selector += type;
            selector.push_back('"');
        }
        selector += R"(],"scope_kinds":[],"tool_names":[]})";

        auto rules = std::string{R"([{"approval":null,"decision":"allow",)"
                                 R"("maximum_risk":"medium","priority":20,)"
                                 R"("required_controller_capabilities":[")"};
        rules += k_operateCapability;
        rules += R"("],"rule_id":"allow-routine",)";
        rules += selector;
        rules += R"(},{"approval":{"approver_capability":")";
        rules += k_approveCapability;
        rules += R"(","bind_exact_effects":true,"single_use":true},)"
                 R"("decision":"require_approval","maximum_risk":"high",)"
                 R"("priority":10,"required_controller_capabilities":[")";
        rules += k_operateCapability;
        rules += R"("],"rule_id":"approve-elevated",)";
        rules += selector;
        rules += "}]";

        auto artifact = std::string{R"({"default_decision":"deny",)"
                                    R"("operator_protocol_schema_hash":")"};
        artifact += operatorProtocolSchemaHash.hex();
        artifact += R"(","ordered_rules":)";
        artifact += rules;
        artifact += R"(,"owned_by":"operator","policy_id":"conformance-fixture",)"
                    R"("policy_version":"1","unknown_effect_decision":"deny"})";
        return artifact;
    }

    // The plan authority a deployment builds. The exact operator protocol bytes
    // are the ones the session manifest is pinned to, and the RuntimeModel
    // binding is the model the Host parsed out of the artifact that manifest
    // pins, so an authority that answers for another schema or another model
    // cannot be created at all.
    //
    // The two readers are the deployment's, not the suite's: the operator
    // protocol is the Operator's own schema and is the same for every project,
    // so what a provider supplies is the documents and what a consumer writes
    // is a ProjectVocabulary and never a JSON reader.
    //
    // What the suite adds is one refusal of its own: the step reader below also
    // refuses a UI-action step naming anything but `uiAction`. A contract run
    // drives exactly one UI action -- the one the project's ProjectVocabulary
    // names -- so a plan that named another would be telling the suite two
    // different things about what this Operation does. That refusal is the
    // suite's and is about agreement with the run; the Operator's own refusal,
    // against the installed model, is in mintStep and holds for production
    // callers that never see this file.
    [[nodiscard]]
    inline auto planAuthority(
        VerifiedProjectRegistration const& registration,
        SessionManifest const& manifest,
        task::RuntimeModelBinding const& runtimeModel,
        std::string_view exactOperatorProtocolSchemaBytes,
        std::string_view exactPolicyArtifactBytes,
        task::UiActionUnderTest const& uiAction
    ) -> Result<OperatorPlanAuthority>
    {
        return OperatorPlanAuthority::create(
            registration,
            manifest,
            runtimeModel,
            exactOperatorProtocolSchemaBytes,
            exactPolicyArtifactBytes,
            deployment::readPlanProposal,
            // Init-capture rather than [uiAction]: capturing the const& parameter
            // by name gives the closure a CONST member, which its move
            // constructor cannot move and must copy instead -- a copy that can
            // throw, out of a move. Deducing the member type here drops the const.
            [uiAction = uiAction](ValidatedDocument const& intent)
                -> Result<StepIntentClaims>
            {
                UF_TRY_VALUE(claims, deployment::readStepIntent(intent));
                if (claims.kind != StepKind::UiAction)
                {
                    return claims;
                }
                if (
                    claims.surfaceId != uiAction.surface
                    || claims.uiTargetId != uiAction.uiTarget
                    || claims.actionId != uiAction.action
                )
                {
                    return fail(
                        AutomationErrorKind::InvalidResource,
                        "a UIActionIntent names a UI action other than the one "
                        "this run agreed on"
                    );
                }
                return claims;
            }
        );
    }
}
