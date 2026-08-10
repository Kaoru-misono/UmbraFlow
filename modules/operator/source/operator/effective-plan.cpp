#include "effective-plan.hpp"

#include <core/error/contracts.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        // The one approval kind the ledger has. OP:`EffectivePlan` types
        // required_approvals as an array of identifiers, and the `approvals`
        // table holds exactly one human token, so the array is either empty or
        // this single name.
        constexpr auto k_humanApproval = std::string_view{"human"};

        struct RiskApproval final
        {
            Risk risk{Risk::Critical};
            bool required{};
        };

        // Which derived risks demand a human before the first Host dispatch.
        // It is Operator-owned and deliberately not read from a policy
        // artifact: nothing parses one yet, and a table nobody can supply is
        // the honest shape of that gap.
        constexpr auto k_approvalByRisk = std::array{
            RiskApproval{.risk = Risk::ReadOnly, .required = false},
            RiskApproval{.risk = Risk::Low, .required = false},
            RiskApproval{.risk = Risk::Medium, .required = false},
            RiskApproval{.risk = Risk::High, .required = true},
            RiskApproval{.risk = Risk::Critical, .required = true},
        };

        [[nodiscard]]
        auto approvalRequiredFor(Risk risk) noexcept -> bool
        {
            auto const found = std::ranges::find(
                k_approvalByRisk,
                risk,
                &RiskApproval::risk
            );
            if (found == k_approvalByRisk.end())
            {
                return true;
            }
            return found->required;
        }

        auto appendHash(std::string& output, ContentHash const& hash) -> void
        {
            appendJsonString(output, hash.hex());
        }

        [[nodiscard]]
        auto requireField(
            std::string_view value,
            std::string_view field
        ) -> Status
        {
            if (value.empty() || !isValidUtf8(value))
            {
                return fail(
                    AutomationErrorKind::InvalidResource,
                    std::string{field} + " must be non-empty valid UTF-8"
                );
            }
            return ok();
        }

        // OP:`ExpectedEffect` as an array, in JCS member order. JCS orders
        // members by UTF-16 code unit, which is why opaque_project_payload
        // precedes payload_schema_hash and scope_key precedes scope_kind.
        [[nodiscard]]
        auto effectsJcs(std::span<ProposedEffect const> effects) -> std::string
        {
            auto output = std::string{"["};
            auto first  = true;
            for (auto const& effect : effects)
            {
                if (!first)
                {
                    output.push_back(',');
                }
                first = false;
                output += "{\"namespaced_type\":";
                appendJsonString(output, effect.namespacedType);
                output += ",\"opaque_project_payload\":";
                output += effect.opaqueProjectPayload;
                output += ",\"payload_schema_hash\":";
                appendHash(output, effect.payloadSchemaHash);
                output += ",\"risk\":";
                appendJsonString(output, riskWireName(effect.risk));
                output += ",\"scope_key\":";
                appendJsonString(output, effect.scopeKey);
                output += ",\"scope_kind\":";
                appendJsonString(output, effect.scopeKind);
                output.push_back('}');
            }
            output.push_back(']');
            return output;
        }

        struct EffectEnvelope final
        {
            std::vector<ProposedEffect> effects{};
            ContentHash                 hash;
        };

        // The declared effects in one determined order, and the hash over that
        // order. JCS does not sort arrays, so without this the same effect set
        // proposed in two orders would produce two envelopes and two approvals
        // for one decision. The rule mirrors the artifact-root rule the frozen
        // authority already fixes: non-empty, unique, sorted by UTF-8 bytes.
        [[nodiscard]]
        auto deriveEffectEnvelope(
            std::vector<ProposedEffect> effects
        ) -> Result<EffectEnvelope>
        {
            auto ordered = std::move(effects);
            for (auto const& effect : ordered)
            {
                UF_TRY(requireField(effect.namespacedType, "effect namespaced_type"));
                UF_TRY(requireField(effect.scopeKind, "effect scope_kind"));
                UF_TRY(requireField(effect.scopeKey, "effect scope_key"));
                UF_TRY(requireField(
                    effect.opaqueProjectPayload,
                    "effect opaque_project_payload"
                ));
            }
            std::ranges::sort(ordered, {}, [](ProposedEffect const& effect) {
                return std::tie(effect.namespacedType, effect.scopeKind, effect.scopeKey);
            });
            for (auto index = std::size_t{1}; index < ordered.size(); ++index)
            {
                auto const& previous = ordered[index - 1U];
                auto const& current  = ordered[index];
                if (
                    previous.namespacedType == current.namespacedType
                    && previous.scopeKind == current.scopeKind
                    && previous.scopeKey == current.scopeKey
                )
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        "PlanProposal declares one effect scope twice"
                    );
                }
            }
            auto const material = effectsJcs(ordered);
            UF_TRY_VALUE(hash, sha256(std::as_bytes(std::span{material})));
            return EffectEnvelope{.effects = std::move(ordered), .hash = hash};
        }

        [[nodiscard]]
        auto highestRisk(std::span<ProposedEffect const> effects) noexcept -> Risk
        {
            auto highest = Risk::ReadOnly;
            for (auto const& effect : effects)
            {
                if (effect.risk > highest)
                {
                    highest = effect.risk;
                }
            }
            return highest;
        }

        [[nodiscard]]
        auto workflowLimitsJcs(WorkflowLimits const& limits) -> std::string
        {
            auto output = std::string{"{\"maximum_dispatches\":"};
            output += std::to_string(limits.maximumDispatches);
            output += ",\"maximum_elapsed_ms\":";
            output += std::to_string(limits.maximumElapsedMillis);
            output += ",\"maximum_observations\":";
            output += std::to_string(limits.maximumObservations);
            output += ",\"maximum_steps\":";
            output += std::to_string(limits.maximumSteps);
            output += ",\"maximum_waits\":";
            output += std::to_string(limits.maximumWaits);
            output.push_back('}');
            return output;
        }

        struct PlanBody final
        {
            std::string_view                toolName{};
            std::string_view                toolVersion{};
            std::string_view                canonicalArgs{};
            ContentHash                     commandFingerprint;
            ContentHash                     projectRegistrationHash;
            ContentHash                     decisionBasisHash;
            std::span<ProposedEffect const> effects{};
            std::span<std::string const>    allowedUiActions{};
            WorkflowLimits                  limits{};
            Risk                            risk{Risk::Critical};
            bool                            approvalRequired{};
        };

        // OP:`EffectivePlan` in JCS member order. plan_hash cannot cover
        // itself, so the hashed form omits it and the stored form carries it at
        // its sorted position; `planHash` selects between the two.
        [[nodiscard]]
        auto planJcs(
            PlanBody const& body,
            ContentHash const* planHash
        ) -> std::string
        {
            auto output = std::string{"{\"allowed_ui_actions\":["};
            auto first  = true;
            for (auto const& action : body.allowedUiActions)
            {
                if (!first)
                {
                    output.push_back(',');
                }
                first = false;
                appendJsonString(output, action);
            }
            output += "],\"canonical_args\":";
            output += body.canonicalArgs;
            output += ",\"command_fingerprint\":";
            appendHash(output, body.commandFingerprint);
            output += ",\"decision_basis_hash\":";
            appendHash(output, body.decisionBasisHash);
            output += ",\"effective_effects\":";
            output += effectsJcs(body.effects);
            if (planHash != nullptr)
            {
                output += ",\"plan_hash\":";
                appendHash(output, *planHash);
            }
            output += ",\"project_registration_hash\":";
            appendHash(output, body.projectRegistrationHash);
            output += ",\"required_approvals\":[";
            if (body.approvalRequired)
            {
                appendJsonString(output, k_humanApproval);
            }
            output += "],\"risk\":";
            appendJsonString(output, riskWireName(body.risk));
            output += ",\"tool_name\":";
            appendJsonString(output, body.toolName);
            output += ",\"tool_version\":";
            appendJsonString(output, body.toolVersion);
            output += ",\"workflow_limits\":";
            output += workflowLimitsJcs(body.limits);
            output.push_back('}');
            return output;
        }

        // Whether the frozen plan's allowed_ui_actions carries this step key.
        //
        // It is not a JSON parser and must not become one: allowed_ui_actions
        // is the first member of the JCS object planJcs emits, its elements are
        // complete JSON string tokens, and the JCS escaping appendJsonString
        // applies is injective over valid UTF-8. So membership is decided by
        // comparing whole escaped tokens, and no element can match a substring
        // of another.
        [[nodiscard]]
        auto frozenPlanAllows(
            std::string_view canonicalPlan,
            std::string_view stepKey
        ) -> bool
        {
            constexpr auto prefix = std::string_view{"{\"allowed_ui_actions\":["};
            if (!canonicalPlan.starts_with(prefix))
            {
                return false;
            }
            auto needle = std::string{};
            appendJsonString(needle, stepKey);

            auto remaining = canonicalPlan.substr(prefix.size());
            while (!remaining.empty() && remaining.front() == '"')
            {
                auto at      = std::size_t{1};
                auto escaped = false;
                while (at < remaining.size())
                {
                    auto const character = remaining[at];
                    ++at;
                    if (escaped)
                    {
                        escaped = false;
                        continue;
                    }
                    if (character == '\\')
                    {
                        escaped = true;
                        continue;
                    }
                    if (character == '"')
                    {
                        break;
                    }
                }
                if (remaining.substr(0U, at) == needle)
                {
                    return true;
                }
                remaining.remove_prefix(at);
                if (!remaining.empty() && remaining.front() == ',')
                {
                    remaining.remove_prefix(1U);
                    continue;
                }
                break;
            }
            return false;
        }

        // The identity of one step at one index. plan_hash already covers the
        // command fingerprint and the decision basis, so a step is bound to the
        // whole world its plan was frozen against; the decimal index is what
        // stops the same document being replayed at another position.
        [[nodiscard]]
        auto deriveStepIntent(
            ContentHash const& planHash,
            uint64 stepIndex,
            std::string_view canonicalStep
        ) -> Result<ContentHash>
        {
            auto material = planHash.hex();
            material.push_back('\0');
            material += std::to_string(stepIndex);
            material.push_back('\0');
            material += canonicalStep;
            return sha256(std::as_bytes(std::span{material}));
        }
    }

    auto riskWireName(Risk risk) noexcept -> std::string_view
    {
        switch (risk)
        {
        case Risk::ReadOnly: return "read_only";
        case Risk::Low: return "low";
        case Risk::Medium: return "medium";
        case Risk::High: return "high";
        case Risk::Critical: return "critical";
        }

        UF_UNREACHABLE_MSG("Unknown Risk value");
    }

    auto stepKindWireName(StepKind kind) noexcept -> std::string_view
    {
        switch (kind)
        {
        case StepKind::UiAction: return "ui_action";
        case StepKind::Wait: return "wait";
        }

        UF_UNREACHABLE_MSG("Unknown StepKind value");
    }

    EffectivePlan::EffectivePlan(
        ContentHash projectRegistrationHash,
        ContentHash operatorProtocolSchemaHash,
        ContentHash commandFingerprint,
        ContentHash decisionBasisHash,
        ContentHash effectEnvelopeHash,
        ContentHash planHash,
        std::string operationId,
        std::string toolName,
        std::string toolVersion,
        std::string canonicalPlan,
        std::vector<ProposedEffect> effects,
        std::vector<std::string> allowedUiActions,
        WorkflowLimits limits,
        Risk risk,
        bool approvalRequired
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_operatorProtocolSchemaHash{operatorProtocolSchemaHash}
        , m_commandFingerprint{commandFingerprint}
        , m_decisionBasisHash{decisionBasisHash}
        , m_effectEnvelopeHash{effectEnvelopeHash}
        , m_planHash{planHash}
        , m_operationId{std::move(operationId)}
        , m_toolName{std::move(toolName)}
        , m_toolVersion{std::move(toolVersion)}
        , m_canonicalPlan{std::move(canonicalPlan)}
        , m_effects{std::move(effects)}
        , m_allowedUiActions{std::move(allowedUiActions)}
        , m_limits{limits}
        , m_risk{risk}
        , m_approvalRequired{approvalRequired}
    {
    }

    auto EffectivePlan::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto EffectivePlan::operatorProtocolSchemaHash() const -> ContentHash
    {
        return m_operatorProtocolSchemaHash;
    }

    auto EffectivePlan::commandFingerprint() const -> ContentHash
    {
        return m_commandFingerprint;
    }

    auto EffectivePlan::decisionBasisHash() const -> ContentHash
    {
        return m_decisionBasisHash;
    }

    auto EffectivePlan::effectEnvelopeHash() const -> ContentHash
    {
        return m_effectEnvelopeHash;
    }

    auto EffectivePlan::planHash() const -> ContentHash
    {
        return m_planHash;
    }

    auto EffectivePlan::risk() const noexcept -> Risk
    {
        return m_risk;
    }

    auto EffectivePlan::approvalRequired() const noexcept -> bool
    {
        return m_approvalRequired;
    }

    auto EffectivePlan::operationId() const noexcept -> std::string const&
    {
        return m_operationId;
    }

    auto EffectivePlan::toolName() const noexcept -> std::string const&
    {
        return m_toolName;
    }

    auto EffectivePlan::toolVersion() const noexcept -> std::string const&
    {
        return m_toolVersion;
    }

    auto EffectivePlan::canonicalPlan() const noexcept -> std::string const&
    {
        return m_canonicalPlan;
    }

    auto EffectivePlan::effects() const noexcept
        -> std::vector<ProposedEffect> const&
    {
        return m_effects;
    }

    auto EffectivePlan::allowedUiActions() const noexcept
        -> std::vector<std::string> const&
    {
        return m_allowedUiActions;
    }

    auto EffectivePlan::limits() const noexcept -> WorkflowLimits
    {
        return m_limits;
    }

    EffectiveStep::EffectiveStep(
        ContentHash planHash,
        ContentHash stepIntentHash,
        std::string operationId,
        std::string stepKey,
        std::string canonicalStep,
        uint64 stepIndex,
        StepKind kind
    )
        : m_planHash{planHash}
        , m_stepIntentHash{stepIntentHash}
        , m_operationId{std::move(operationId)}
        , m_stepKey{std::move(stepKey)}
        , m_canonicalStep{std::move(canonicalStep)}
        , m_stepIndex{stepIndex}
        , m_kind{kind}
    {
    }

    auto EffectiveStep::planHash() const -> ContentHash
    {
        return m_planHash;
    }

    auto EffectiveStep::stepIntentHash() const -> ContentHash
    {
        return m_stepIntentHash;
    }

    auto EffectiveStep::stepIndex() const noexcept -> uint64
    {
        return m_stepIndex;
    }

    auto EffectiveStep::kind() const noexcept -> StepKind
    {
        return m_kind;
    }

    auto EffectiveStep::operationId() const noexcept -> std::string const&
    {
        return m_operationId;
    }

    auto EffectiveStep::stepKey() const noexcept -> std::string const&
    {
        return m_stepKey;
    }

    auto EffectiveStep::canonicalStep() const noexcept -> std::string const&
    {
        return m_canonicalStep;
    }

    OperatorPlanAuthority::OperatorPlanAuthority(
        ContentHash projectRegistrationHash,
        ContentHash operatorProtocolSchemaHash,
        PlanProposalReader readProposal,
        StepIntentReader readStepIntent
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_operatorProtocolSchemaHash{operatorProtocolSchemaHash}
        , m_readProposal{std::move(readProposal)}
        , m_readStepIntent{std::move(readStepIntent)}
    {
    }

    auto OperatorPlanAuthority::create(
        VerifiedProjectRegistration const& registration,
        SessionManifest const& sessionManifest,
        std::string_view exactOperatorProtocolSchemaBytes,
        PlanProposalReader readProposal,
        StepIntentReader readStepIntent
    ) -> Result<OperatorPlanAuthority>
    {
        if (!readProposal || !readStepIntent)
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "OperatorPlanAuthority requires both operator protocol readers"
            );
        }
        if (sessionManifest.projectRegistrationHash() != registration.hash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "SessionManifest is pinned to a different ProjectRegistration"
            );
        }
        UF_TRY_VALUE(
            schemaHash,
            sha256(std::as_bytes(std::span{exactOperatorProtocolSchemaBytes}))
        );
        if (schemaHash != sessionManifest.operatorProtocolSchemaHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "Operator protocol schema bytes do not match the pinned session manifest"
            );
        }
        return OperatorPlanAuthority{
            registration.hash(),
            schemaHash,
            std::move(readProposal),
            std::move(readStepIntent),
        };
    }

    auto OperatorPlanAuthority::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto OperatorPlanAuthority::mintPlan(
        PlanMintInputs const& inputs
    ) const -> Result<EffectivePlan>
    {
        if (m_projectRegistrationHash != inputs.projectRegistrationHash)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Plan authority is bound to a different ProjectRegistration"
            );
        }
        if (
            inputs.proposal.projectRegistrationHash() != m_projectRegistrationHash
            || inputs.proposal.function() != ProjectPluginFunction::Plan
            || inputs.proposal.direction() != ProjectDocumentDirection::Output
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal was not produced by this registration's plan function"
            );
        }
        UF_TRY_VALUE(claims, m_readProposal(inputs.proposal.bytes()));
        if (claims.toolName != inputs.toolName)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal names a tool the Operation was not created for"
            );
        }
        if (claims.toolVersion != inputs.toolVersion)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal names a tool version the Tool Catalog did not decide"
            );
        }
        if (claims.canonicalArgs != inputs.canonicalArgs)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal names arguments the Operation was not created for"
            );
        }

        auto allowed = std::move(claims.allowedUiActions);
        for (auto const& action : allowed)
        {
            UF_TRY(requireField(action, "allowed_ui_actions entry"));
        }
        std::ranges::sort(allowed);
        allowed.erase(std::ranges::unique(allowed).begin(), allowed.end());

        UF_TRY_VALUE(envelope, deriveEffectEnvelope(std::move(claims.effects)));

        // Every bound is a minimum against the ceiling, so widening is
        // arithmetically impossible rather than policy-checked.
        auto const limits = WorkflowLimits{
            .maximumSteps = std::min(
                claims.limits.maximumSteps,
                k_workflowCeiling.maximumSteps
            ),
            .maximumDispatches = std::min(
                claims.limits.maximumDispatches,
                k_workflowCeiling.maximumDispatches
            ),
            .maximumObservations = std::min(
                claims.limits.maximumObservations,
                k_workflowCeiling.maximumObservations
            ),
            .maximumWaits = std::min(
                claims.limits.maximumWaits,
                k_workflowCeiling.maximumWaits
            ),
            .maximumElapsedMillis = std::min(
                claims.limits.maximumElapsedMillis,
                k_workflowCeiling.maximumElapsedMillis
            ),
        };
        if (
            limits.maximumSteps == 0U
            || limits.maximumDispatches == 0U
            || limits.maximumObservations == 0U
            || limits.maximumElapsedMillis == 0U
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal workflow_limits leave no step, dispatch or budget"
            );
        }

        auto const risk             = highestRisk(envelope.effects);
        auto const approvalRequired = approvalRequiredFor(risk);
        auto const body             = PlanBody{
            .toolName                = inputs.toolName,
            .toolVersion             = inputs.toolVersion,
            .canonicalArgs           = inputs.canonicalArgs,
            .commandFingerprint      = inputs.commandFingerprint,
            .projectRegistrationHash = m_projectRegistrationHash,
            .decisionBasisHash       = inputs.decisionBasisHash,
            .effects                 = envelope.effects,
            .allowedUiActions        = allowed,
            .limits                  = limits,
            .risk                    = risk,
            .approvalRequired        = approvalRequired,
        };
        auto const hashedForm = planJcs(body, nullptr);
        UF_TRY_VALUE(planHash, sha256(std::as_bytes(std::span{hashedForm})));
        return EffectivePlan{
            m_projectRegistrationHash,
            m_operatorProtocolSchemaHash,
            inputs.commandFingerprint,
            inputs.decisionBasisHash,
            envelope.hash,
            planHash,
            inputs.operationId,
            inputs.toolName,
            inputs.toolVersion,
            planJcs(body, &planHash),
            std::move(envelope.effects),
            std::move(allowed),
            limits,
            risk,
            approvalRequired,
        };
    }

    auto OperatorPlanAuthority::mintStep(
        StepMintInputs const& inputs
    ) const -> Result<EffectiveStep>
    {
        if (
            inputs.intent.projectRegistrationHash() != m_projectRegistrationHash
            || inputs.intent.function() != ProjectPluginFunction::NextStep
            || inputs.intent.direction() != ProjectDocumentDirection::Output
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step intent was not produced by this registration's next_step function"
            );
        }
        UF_TRY_VALUE(claims, m_readStepIntent(inputs.intent.bytes()));
        UF_TRY(requireField(claims.stepKey, "step_key"));
        if (!frozenPlanAllows(inputs.canonicalPlan, claims.stepKey))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step intent names an action outside the frozen allowed set"
            );
        }
        UF_TRY_VALUE(
            stepIntentHash,
            deriveStepIntent(
                inputs.planHash,
                inputs.stepIndex,
                inputs.intent.bytes()
            )
        );
        return EffectiveStep{
            inputs.planHash,
            stepIntentHash,
            inputs.operationId,
            claims.stepKey,
            inputs.intent.bytes(),
            inputs.stepIndex,
            claims.kind,
        };
    }
}
