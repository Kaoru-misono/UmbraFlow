#include "effective-plan.hpp"

#include <core/error/contracts.hpp>
#include <core/numeric/checked-arithmetic.hpp>
#include <core/numeric/checked-cast.hpp>
#include <core/text/json-text.hpp>
#include <core/text/utf8.hpp>

#include <json/value.hpp>

#include <domain/error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace uf::operator_runtime
{
    namespace
    {
        constexpr auto k_operatorPlanErrorCodes = std::array{
            OperatorPlanErrorCode::PlannedUiTargetSubstituted,
            OperatorPlanErrorCode::PlannedArgumentContradicted,
        };

        [[nodiscard]]
        auto operatorPlanErrorDetailValue(
            OperatorPlanErrorCode code
        ) noexcept -> int
        {
            auto const underlying = checkedCast<int>(std::to_underlying(code));
            UF_CHECK(underlying.has_value());
            auto const encoded = checkedAdd(*underlying, 1);
            UF_CHECK(encoded.has_value());
            return *encoded;
        }

        class OperatorPlanErrorCategory final : public std::error_category
        {
        public:
            [[nodiscard]] auto name() const noexcept -> char const* override
            {
                return "uf.operator.plan-authority";
            }

            [[nodiscard]] auto message(int value) const -> std::string override
            {
                for (auto const code : k_operatorPlanErrorCodes)
                {
                    if (operatorPlanErrorDetailValue(code) == value)
                    {
                        return std::string{operatorPlanErrorWireName(code)};
                    }
                }
                return "UnknownOperatorPlanErrorCode";
            }
        };

        [[nodiscard]]
        auto operatorPlanErrorCategory() noexcept -> std::error_category const&
        {
            static auto const s_category = OperatorPlanErrorCategory{};
            return s_category;
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

        // One member of OP:`UIActionIntent`.action, and the vocabulary the
        // installed RuntimeModel declares for it. The three rows are a table
        // rather than three tests so that adding a fourth identifier to the
        // intent adds a row and cannot add a silently unchecked member.
        //
        // Its views borrow the binding and the claims for the length of one
        // requireDeclaredUi call, which builds the table, reads it and destroys
        // it before returning. Nothing stores one and none may be returned.
        struct DeclaredUiCheck final
        {
            std::span<std::string const> declared{};
            std::string_view             named{};
            std::string_view             field{};
        };

        // The whole of what the Operator asks about a RuntimeModel: whether
        // three strings the plugin wrote are strings the trusted parser
        // published. It reads no RuntimeModel field, knows nothing of what a
        // surface or an action is, and would answer identically if the model
        // were a list of colours -- which is why this is identity and not
        // interpretation of RuntimeModel semantics.
        //
        // A UI-action step whose intent left an identifier empty is refused
        // here too: an empty name is in no vocabulary, and a reader that never
        // filled these members therefore stops every UI-action step instead of
        // passing a test against nothing.
        [[nodiscard]]
        auto requireDeclaredUi(
            task::RuntimeModelBinding const& runtimeModel,
            std::string_view runtimeArtifactRootHash,
            StepIntentClaims const& claims
        ) -> Status
        {
            if (runtimeModel.artifactRootHash().hex() != runtimeArtifactRootHash)
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "Plan authority answers for a RuntimeArtifact this session "
                    "did not pin"
                );
            }
            auto const& declared = runtimeModel.declaredUi();
            auto const checks    = std::array{
                DeclaredUiCheck{declared.surfaces, claims.surfaceId, "surface_id"},
                DeclaredUiCheck{
                    declared.uiTargets,
                    claims.uiTargetId,
                    "ui_target_id",
                },
                DeclaredUiCheck{declared.actions, claims.actionId, "action_id"},
            };
            for (auto const& check : checks)
            {
                UF_TRY(requireField(check.named, check.field));
                if (!std::ranges::contains(check.declared, check.named))
                {
                    return fail(
                        AutomationErrorKind::ActionRejected,
                        std::string{"UIActionIntent names a "}
                            + std::string{check.field}
                            + " the installed RuntimeModel does not define"
                    );
                }
            }
            return ok();
        }

        // OP:`EffectEnvelope` as an array, in JCS member order. JCS orders
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

        // The plan's effective_effects and the hash the frozen plan publishes
        // over them as effect_envelope_hash. It is deliberately not called an
        // EffectEnvelope: that name belongs to OP:`EffectEnvelope`, which is
        // ONE effect, and this is the whole ordered set with its digest.
        struct EffectiveEffects final
        {
            std::vector<ProposedEffect> effects{};
            ContentHash                 hash;
        };

        // The declared effects in one determined order, and the hash over that
        // order. JCS does not sort arrays, so without this the same effect set
        // proposed in two orders would produce two digests and two approvals
        // for one decision. The rule mirrors the artifact-root rule the frozen
        // authority already fixes: non-empty, unique, sorted by UTF-8 bytes.
        [[nodiscard]]
        auto deriveEffectiveEffects(
            std::vector<ProposedEffect> effects
        ) -> Result<EffectiveEffects>
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
            return EffectiveEffects{.effects = std::move(ordered), .hash = hash};
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
            std::span<std::string const>    requiredApprovals{};
            WorkflowLimits                  limits{};
            Risk                            risk{Risk::Critical};
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
            first = true;
            for (auto const& approver : body.requiredApprovals)
            {
                if (!first)
                {
                    output.push_back(',');
                }
                first = false;
                appendJsonString(output, approver);
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

        // Every declared ui_target this value names, at any depth, as a string
        // it holds rather than as a member name it spells.
        //
        // This is the same question requireDeclaredUi asks, pointed at the
        // other document: whether a string here is a string the trusted
        // RuntimeModel parser published. It reads no project meaning out of
        // canonical_args and cannot -- it does not know which argument is the
        // target, and never asks. A member name is deliberately not a hit: a
        // plan whose arguments are keyed by a ui_target has still not chosen
        // one, and treating a key as a choice would let an unrelated shape
        // widen what a step may aim at.
        //
        // Recursion is bounded because json::parse is: it refuses beyond 64
        // levels, so no accepted document can drive this deeper than that.
        [[nodiscard]]
        auto declaredUiTargetsIn(
            json::Value const& value,
            std::span<std::string const> declared
        ) -> std::vector<std::string>
        {
            auto named = std::vector<std::string>{};
            switch (value.kind())
            {
            case json::ValueKind::Null:
            case json::ValueKind::Boolean:
            case json::ValueKind::Number:
                return named;
            case json::ValueKind::String:
                if (std::ranges::contains(declared, value.string()))
                {
                    named.emplace_back(value.string());
                }
                return named;
            case json::ValueKind::Array:
                for (auto const& item : value.items())
                {
                    auto found = declaredUiTargetsIn(item, declared);
                    named.insert(
                        named.end(),
                        std::make_move_iterator(found.begin()),
                        std::make_move_iterator(found.end())
                    );
                }
                return named;
            case json::ValueKind::Object:
                for (auto const& member : value.members())
                {
                    auto found = declaredUiTargetsIn(member.second, declared);
                    named.insert(
                        named.end(),
                        std::make_move_iterator(found.begin()),
                        std::make_move_iterator(found.end())
                    );
                }
                return named;
            }

            UF_UNREACHABLE_MSG("Unknown json::ValueKind value");
        }

        // The frozen plan's canonical_args, read out of the plan once because
        // both rules below judge a step against them. The operator protocol
        // types the member as any canonical JSON value, so this returns the
        // value whatever kind it is and neither rule may assume an object.
        [[nodiscard]]
        auto frozenPlanArguments(
            std::string_view canonicalPlan
        ) -> Result<json::Value>
        {
            UF_TRY_VALUE(plan, json::parse(canonicalPlan));
            auto const* p_arguments = plan.find("canonical_args");
            if (p_arguments == nullptr)
            {
                return fail(
                    AutomationErrorKind::InternalInvariant,
                    "frozen plan carries no canonical_args"
                );
            }
            return *p_arguments;
        }

        // Whether this step may aim where it says it does.
        //
        // The frozen plan's canonical_args are the Operation's own command
        // arguments; mintPlan refuses a proposal that restates them
        // differently, so they are what the caller chose and not what the
        // plugin later decided. Where they name UI targets the installed
        // RuntimeModel declares, those are the objects this plan is about, and
        // a step aiming outside them is acting on something nobody planned --
        // which the reconciliation afterwards would then judge against the
        // planned object rather than the touched one.
        //
        // A plan whose arguments name no declared ui_target chose no object,
        // so there is nothing here to contradict and the step's target is
        // bounded by the RuntimeModel vocabulary alone. That is a real case
        // and not a relaxation, but it is also the limit of what this rule can
        // decide: which argument holds the target is declared per tool on the
        // project side and never crosses into the Operator, so a plan naming
        // two objects leaves a step free to aim at either.
        //
        // This rule owns action.ui_target_id, and requirePlannedArgumentValues
        // below owns action.canonical_parameters. They are two obligations and
        // not two spellings of one, because the two members are not the same
        // kind of statement: a ui_target_id is a bare identifier carrying no
        // member name, so nothing can match it against the plan except the
        // plan's VALUES, while canonical_parameters carries member names and is
        // therefore matched BY MEMBER. Neither comparison can be run on the
        // other member, and neither rule can see what the other sees: this one
        // cannot notice a contradicted argument, and that one is silent
        // whenever the step restates no planned member at all -- silence the
        // same plugin whose choice is in question can help itself to, by
        // restating nothing, and silence that is structural whenever the plan's
        // canonical_args are not an object.
        [[nodiscard]]
        auto requirePlannedUiTarget(
            json::Value const& plannedArguments,
            std::span<std::string const> declaredUiTargets,
            std::string_view uiTargetId
        ) -> Status
        {
            auto const planned = declaredUiTargetsIn(
                plannedArguments,
                declaredUiTargets
            );
            if (planned.empty() || std::ranges::contains(planned, uiTargetId))
            {
                return ok();
            }
            return fail(
                OperatorPlanErrorCode::PlannedUiTargetSubstituted,
                "UIActionIntent aims at a ui_target the frozen plan did not choose"
            );
        }

        // Whether this step's own parameters agree with the plan they belong
        // to: every member the step restates must carry the value the frozen
        // plan's canonical_args gave that member.
        //
        // That decides the target exactly without anyone naming which member
        // holds it. The declarative workflow adapter re-resolves the target
        // from the fresh observation and writes it back as
        // canonical_parameters[target_argument] -- see the comment above
        // resolve_target in declarative-workflow-tool.cpp -- so a re-resolution
        // that lands on another instance contradicts the member the caller
        // stated, and is refused for contradicting it rather than for being a
        // target, which the Operator still cannot recognise.
        //
        // A member the step restates that the plan never named is outside this
        // rule and is accepted. canonical_parameters are a UI action's
        // parameters and canonical_args are a command's arguments: they share a
        // member name only where a project chose to, and the step intent schema
        // says of canonical_parameters that no member of ProjectRegistrationClaims
        // pins a schema for it and this framework has no authority to invent
        // one. Refusing an unstated member would be exactly that invention --
        // "every action parameter must have been a command argument" -- and
        // would forbid every UI action that takes a parameter of its own. It
        // would also judge one plugin statement against nothing, where the whole
        // warrant for this rule is the caller's stated choice outranking the
        // plugin's later one.
        //
        // Neither value being an object is the same answer for the same reason:
        // a non-object names no member, so no member is restated and there is
        // nothing to compare. It is the rule read literally and not a fallback
        // branch -- requiring an object here would be the Operator narrowing a
        // protocol member the protocol deliberately leaves open.
        [[nodiscard]]
        auto requirePlannedArgumentValues(
            json::Value const& plannedArguments,
            std::string_view canonicalParameters
        ) -> Status
        {
            UF_TRY_VALUE(parameters, json::parse(canonicalParameters));
            if (
                plannedArguments.kind() != json::ValueKind::Object
                || parameters.kind() != json::ValueKind::Object
            )
            {
                return ok();
            }
            for (auto const& parameter : parameters.members())
            {
                auto const* p_planned = plannedArguments.find(parameter.first);
                if (p_planned == nullptr)
                {
                    continue;
                }
                if (*p_planned != parameter.second)
                {
                    return fail(
                        OperatorPlanErrorCode::PlannedArgumentContradicted,
                        "UIActionIntent restates a planned argument with a value "
                        "the frozen plan did not give it"
                    );
                }
            }
            return ok();
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

    auto operatorPlanErrorCode(
        Error const& error
    ) noexcept -> std::optional<OperatorPlanErrorCode>
    {
        auto const detail = error.detailCode();
        if (detail.category() != operatorPlanErrorCategory())
        {
            return std::nullopt;
        }
        for (auto const code : k_operatorPlanErrorCodes)
        {
            if (operatorPlanErrorDetailValue(code) == detail.value())
            {
                return code;
            }
        }
        return std::nullopt;
    }

    auto operatorPlanErrorWireName(
        OperatorPlanErrorCode code
    ) noexcept -> std::string_view
    {
        switch (code)
        {
        case OperatorPlanErrorCode::PlannedUiTargetSubstituted:
            return "PlannedUiTargetSubstituted";
        case OperatorPlanErrorCode::PlannedArgumentContradicted:
            return "PlannedArgumentContradicted";
        }

        UF_UNREACHABLE_MSG("Unknown OperatorPlanErrorCode value");
    }

    auto fail(
        OperatorPlanErrorCode code,
        std::string message,
        std::source_location location
    ) -> std::unexpected<Error>
    {
        return uf::fail(
            std::error_code{
                operatorPlanErrorDetailValue(code),
                operatorPlanErrorCategory(),
            },
            std::move(message),
            {},
            location
        );
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
        std::vector<std::string> requiredApprovals,
        WorkflowLimits limits,
        Risk risk
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
        , m_requiredApprovals{std::move(requiredApprovals)}
        , m_limits{limits}
        , m_risk{risk}
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

    auto EffectivePlan::requiredApprovals() const noexcept
        -> std::vector<std::string> const&
    {
        return m_requiredApprovals;
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
        task::RuntimeModelBinding runtimeModel,
        VerifiedPolicyArtifact policy,
        PlanProposalReader readProposal,
        StepIntentReader readStepIntent
    )
        : m_projectRegistrationHash{projectRegistrationHash}
        , m_operatorProtocolSchemaHash{operatorProtocolSchemaHash}
        , m_runtimeModel{std::move(runtimeModel)}
        , m_policy{std::move(policy)}
        , m_readProposal{std::move(readProposal)}
        , m_readStepIntent{std::move(readStepIntent)}
    {
    }

    auto OperatorPlanAuthority::create(
        VerifiedProjectRegistration const& registration,
        SessionManifest const& sessionManifest,
        task::RuntimeModelBinding const& runtimeModel,
        std::string_view exactOperatorProtocolSchemaBytes,
        std::string_view exactPolicyArtifactBytes,
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
        if (runtimeModel.artifactRootHash() != sessionManifest.runtimeModelArtifactRootHash())
        {
            return fail(
                AutomationErrorKind::InvalidResource,
                "RuntimeModel binding was parsed from a RuntimeArtifact this "
                "session manifest does not pin"
            );
        }
        UF_TRY_VALUE(
            policy,
            VerifiedPolicyArtifact::verifyExact(
                sessionManifest,
                exactPolicyArtifactBytes
            )
        );
        return OperatorPlanAuthority{
            registration.hash(),
            schemaHash,
            runtimeModel,
            std::move(policy),
            std::move(readProposal),
            std::move(readStepIntent),
        };
    }

    auto OperatorPlanAuthority::projectRegistrationHash() const -> ContentHash
    {
        return m_projectRegistrationHash;
    }

    auto OperatorPlanAuthority::policyHash() const -> ContentHash
    {
        return m_policy.hash();
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
        // Unfalsifiable as the ledger stands: it reads the proposal back from
        // the plugin whose schema owner is bound to this same registration, so
        // both sides move together and deleting this turns nothing red. It is
        // the authority's own binding of the document to the registration, and
        // the check that notices the day a second path reaches a mint.
        if (inputs.proposal.projectRegistrationHash() != m_projectRegistrationHash)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal was not produced by this registration"
            );
        }

        // Which ProjectPlugin function stamped the document is the reader's
        // refusal and not repeated here: the reader is what interprets these
        // bytes as a PlanProposal, and a document stamped for another function
        // would reach a member the operator protocol never put in it.
        UF_TRY_VALUE(claims, m_readProposal(inputs.proposal));
        if (claims.toolName != inputs.toolName)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal names a tool the Operation was not created for"
            );
        }
        if (claims.toolVersion != inputs.descriptor.toolVersion)
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
            if (!std::ranges::contains(inputs.descriptor.uiActionBounds, action))
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "PlanProposal allows UI action " + action
                        + ", which this tool's ui_action_bounds do not declare"
                );
            }
        }
        std::ranges::sort(allowed);
        allowed.erase(std::ranges::unique(allowed).begin(), allowed.end());

        UF_TRY_VALUE(effective, deriveEffectiveEffects(std::move(claims.effects)));
        if (effective.effects.empty())
        {
            // A mutating plan with no effect would reach the policy with
            // nothing to judge, and every rule in an artifact speaks about an
            // effect. Refusing here is what keeps "policy decides" true rather
            // than true of the plans that happen to declare something.
            return fail(
                AutomationErrorKind::ActionRejected,
                "PlanProposal for a mutating tool declares no effect at all"
            );
        }
        for (auto const& effect : effective.effects)
        {
            UF_TRY(effectWithinBounds(inputs.descriptor, effect));
        }

        // Every bound is a minimum against the tool's own declaration, so
        // widening is arithmetically impossible rather than policy-checked.
        // There is no second, compiled-in ceiling beside it: the catalog states
        // this tool's limit and the catalog bytes are inside
        // project_registration_hash.
        auto const& declared = inputs.descriptor.limits;
        auto const  limits   = WorkflowLimits{
            .maximumSteps = std::min(
                claims.limits.maximumSteps,
                declared.maximumSteps
            ),
            .maximumDispatches = std::min(
                claims.limits.maximumDispatches,
                declared.maximumDispatches
            ),
            .maximumObservations = std::min(
                claims.limits.maximumObservations,
                declared.maximumObservations
            ),
            .maximumWaits = std::min(
                claims.limits.maximumWaits,
                declared.maximumWaits
            ),
            .maximumElapsedMillis = std::min(
                claims.limits.maximumElapsedMillis,
                declared.maximumElapsedMillis
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

        // The one place a plan's authorization is decided, and it is an
        // evaluation of the artifact this session pinned rather than a table
        // compiled in beside it.
        UF_TRY_VALUE(
            verdict,
            m_policy.evaluate(PolicyRequest{
                .effects                = effective.effects,
                .controllerCapabilities = inputs.controllerCapabilities,
                .toolName               = inputs.toolName,
            })
        );

        auto const risk = highestRisk(effective.effects);
        auto const body = PlanBody{
            .toolName                = inputs.toolName,
            .toolVersion             = inputs.descriptor.toolVersion,
            .canonicalArgs           = inputs.canonicalArgs,
            .commandFingerprint      = inputs.commandFingerprint,
            .projectRegistrationHash = m_projectRegistrationHash,
            .decisionBasisHash       = inputs.decisionBasisHash,
            .effects                 = effective.effects,
            .allowedUiActions        = allowed,
            .requiredApprovals       = verdict.requiredApprovals,
            .limits                  = limits,
            .risk                    = risk,
        };
        auto const hashedForm = planJcs(body, nullptr);
        UF_TRY_VALUE(planHash, sha256(std::as_bytes(std::span{hashedForm})));
        return EffectivePlan{
            m_projectRegistrationHash,
            m_operatorProtocolSchemaHash,
            inputs.commandFingerprint,
            inputs.decisionBasisHash,
            effective.hash,
            planHash,
            inputs.operationId,
            inputs.toolName,
            inputs.descriptor.toolVersion,
            planJcs(body, &planHash),
            std::move(effective.effects),
            std::move(allowed),
            std::move(verdict.requiredApprovals),
            limits,
            risk,
        };
    }

    auto OperatorPlanAuthority::mintStep(
        StepMintInputs const& inputs
    ) const -> Result<EffectiveStep>
    {
        if (inputs.intent.projectRegistrationHash() != m_projectRegistrationHash)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step intent was not produced by this registration"
            );
        }
        UF_TRY_VALUE(claims, m_readStepIntent(inputs.intent));
        UF_TRY(requireField(claims.stepKey, "step_key"));
        if (!frozenPlanAllows(inputs.canonicalPlan, claims.stepKey))
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step intent names an action outside the frozen allowed set"
            );
        }
        if (
            claims.timeout.maximumElapsedMillis
            > inputs.descriptor.timeout.maximumElapsedMillis
        )
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step intent allows more elapsed time than this tool's "
                "timeout_policy declares"
            );
        }
        if (claims.timeout.onTimeout != inputs.descriptor.timeout.onTimeout)
        {
            return fail(
                AutomationErrorKind::ActionRejected,
                "Step intent on_timeout differs from this tool's timeout_policy"
            );
        }
        if (claims.kind == StepKind::UiAction)
        {
            if (
                !deliveryClassWithin(
                    claims.deliveryClass,
                    inputs.descriptor.idempotency
                )
            )
            {
                return fail(
                    AutomationErrorKind::ActionRejected,
                    "UIActionIntent claims a delivery_class safer than this "
                    "tool's declared idempotency"
                );
            }
            UF_TRY(requireDeclaredUi(
                m_runtimeModel,
                inputs.runtimeArtifactRootHash,
                claims
            ));
            UF_TRY(requireField(
                claims.canonicalParameters,
                "action canonical_parameters"
            ));

            // After the vocabulary and not before it: a name outside the
            // installed model is refused for being outside it, so this
            // refusal only ever means the step named a real ui_target that
            // is not the one the plan chose.
            //
            // The aim is judged before the parameters so that each refusal
            // keeps one meaning: a step that both aims elsewhere and restates
            // the caller's argument as that other object is reported as the
            // substituted target it is, and PlannedArgumentContradicted is
            // only ever seen where the aim was the planned one.
            UF_TRY_VALUE(planned, frozenPlanArguments(inputs.canonicalPlan));
            UF_TRY(requirePlannedUiTarget(
                planned,
                m_runtimeModel.declaredUi().uiTargets,
                claims.uiTargetId
            ));
            UF_TRY(requirePlannedArgumentValues(
                planned,
                claims.canonicalParameters
            ));
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
