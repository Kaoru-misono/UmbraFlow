#pragma once

#include "manifest.hpp"
#include "policy.hpp"
#include "project-plugin.hpp"
#include "tool-descriptor.hpp"

#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <optional>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class OperatorCoordinator;
    class OperatorPlanAuthority;

    // The refusals the step mint makes that a caller has to be able to tell
    // apart. Every other refusal at this boundary is an
    // AutomationErrorKind::ActionRejected carrying a sentence, and a case can
    // only recognise one of those by matching words in prose; a rule whose
    // whole point is that it can be falsified needs a name asked for by
    // identity. The family is the ProjectObservationErrorCode shape, for the
    // same reason that one exists.
    enum class OperatorPlanErrorCode : uint8
    {
        // A UI-action step's canonical_parameters restate a member of the
        // frozen plan's canonical_args with another value. The plan's
        // canonical_args are the Operation's own command arguments, not the
        // plugin's -- mintPlan refuses a proposal that restates them
        // differently -- so this is the caller's stated choice outranking the
        // plugin's later one, and never one plugin statement outranking
        // another.
        //
        // The contradicted member need not hold a ui_target at all -- a
        // quantity, a mode, any argument the caller stated -- so the code
        // names the act, not the member. Which member holds the target is a
        // project-tier declaration that never reaches the Operator, which is
        // exactly why this rule names no member and judges every one the step
        // chose to restate. Since U2b the step's own ui_target_id is an
        // observed instance id the plan cannot name (instances are minted
        // after the plan freezes), so the caller's choice of object is
        // enforced here, on the parameters that can carry a re-resolved
        // object, and never on the step's target member.
        PlannedArgumentContradicted,
    };

    [[nodiscard]]
    auto operatorPlanErrorCode(
        Error const& error
    ) noexcept -> std::optional<OperatorPlanErrorCode>;

    [[nodiscard]]
    auto operatorPlanErrorWireName(
        OperatorPlanErrorCode code
    ) noexcept -> std::string_view;

    [[nodiscard]]
    auto fail(
        OperatorPlanErrorCode code,
        std::string message,
        std::source_location location = std::source_location::current()
    ) -> std::unexpected<Error>;

    // The two shapes a workflow step can take: OP:`UIActionIntent` and
    // OP:`WaitIntent`. A wait never reaches Host dispatch, so the kind decides
    // which budget the step spends and whether a dispatch may name it.
    enum class StepKind : uint8
    {
        UiAction,
        Wait,
    };

    [[nodiscard]] auto stepKindWireName(StepKind kind) noexcept -> std::string_view;

    // What the Operator reads out of a PlanProposal the operator protocol
    // schema has already accepted. Like ProjectRegistrationClaims this is not
    // a construction spec: no caller can hand one to the plan authority.
    struct PlanProposalClaims final
    {
        std::string                 toolName{};
        std::string                 toolVersion{};
        std::string                 canonicalArgs{};
        std::vector<ProposedEffect> effects{};
        std::vector<std::string>    allowedUiActions{};
        WorkflowLimits              limits{};
    };

    // What the Operator reads out of one next_step output.
    //
    // The three UI identifiers are OP:`UIActionIntent`.action, and they are
    // required for a UiAction step: mintStep refuses one that leaves any of them
    // empty. That is why they carry no meaning-bearing default -- a reader that
    // forgot to fill them refuses every UI-action step loudly, rather than
    // passing a membership test against nothing. A Wait names no UI and leaves
    // all three empty.
    //
    // uiTargetId is not held to a vocabulary rule: since U2b it is the
    // observed instance id the step acts on, minted by the Operator and
    // resolved by the observation gate in mintNextStep, and the model's
    // declared ui_targets are what a Host resolves a delivery against. The
    // caller's choice of object is enforced on the parameters instead, which
    // are the one member that can carry a re-resolved object.
    struct StepIntentClaims final
    {
        std::string stepKey{};
        std::string surfaceId{};
        std::string uiTargetId{};
        std::string actionId{};

        // OP:`UIActionIntent`.action.canonical_parameters, as the exact
        // canonical bytes of that value and not a parsed shape: it is compared
        // against the frozen plan's canonical_args, which the Operator also
        // holds as bytes, and the operator protocol types both as any canonical
        // JSON value, so neither may be narrowed to an object here.
        //
        // Every member of it that the plan's canonical_args also name must
        // carry the value the plan gave that member, or the step is refused as
        // PlannedArgumentContradicted.
        //
        // Required for a UiAction step and empty for a Wait, for the reason the
        // three identifiers above carry no meaning-bearing default: no
        // canonical JSON value is the empty string, so an empty one can only
        // mean a reader did not fill it, and mintStep refuses it rather than
        // letting a member comparison pass against nothing.
        std::string canonicalParameters{};

        // Both intents carry a timeout policy, and mintStep holds it to the
        // descriptor's: a step that could outlast what its tool declared would
        // be a per-tool bound with no consumer.
        TimeoutPolicy timeout{};

        // OP:`UIActionIntent`.delivery_class. A Wait names none and leaves this
        // at the weakest claim, which is what an unstated one must read as.
        DeliveryClass deliveryClass{DeliveryClass::NonIdempotent};

        StepKind kind{StepKind::Wait};
    };

    // Trusted deployment callbacks. Each reads a document a ProjectSchemaOwner
    // has already accepted -- exact RFC 8785 JCS, judged against the complete
    // operator protocol definition -- so neither applies either rule again. The
    // parameter type is what carries that: ValidatedDocument's constructor is
    // private to the schema owner, so a caller holding no owner cannot supply
    // one, which is correct because such a caller has no authority to mint a
    // plan either.
    //
    // The one claim the type does not carry is which ProjectPlugin function and
    // direction the document was stamped for, since a Reduce output is the same
    // type. Each reader must refuse a document stamped for another. Neither is
    // passed to plugin code or published in a business VM.
    using PlanProposalReader = std::function<
        Result<PlanProposalClaims>(ValidatedDocument const& proposal)
    >;
    using StepIntentReader = std::function<
        Result<StepIntentClaims>(ValidatedDocument const& intent)
    >;

    // One frozen plan. Only the plan authority bound to the exact
    // ProjectRegistration root and operator_protocol_schema_hash can mint one,
    // and it will only do so for a command fingerprint and decision basis the
    // ledger read out of its own tables.
    class EffectivePlan final
    {
        friend class OperatorPlanAuthority;

        ContentHash                 m_projectRegistrationHash;
        ContentHash                 m_operatorProtocolSchemaHash;
        ContentHash                 m_commandFingerprint;
        ContentHash                 m_decisionBasisHash;
        ContentHash                 m_effectEnvelopeHash;
        ContentHash                 m_planHash;
        std::string                 m_operationId;
        std::string                 m_toolName;
        std::string                 m_toolVersion;
        std::string                 m_canonicalPlan;
        std::vector<ProposedEffect> m_effects;
        std::vector<std::string>    m_allowedUiActions;
        std::vector<std::string>    m_requiredApprovals;
        WorkflowLimits              m_limits;
        Risk                        m_risk;

        EffectivePlan(
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
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto operatorProtocolSchemaHash() const -> ContentHash;
        [[nodiscard]] auto commandFingerprint() const -> ContentHash;
        [[nodiscard]] auto decisionBasisHash() const -> ContentHash;
        [[nodiscard]] auto effectEnvelopeHash() const -> ContentHash;
        [[nodiscard]] auto planHash() const -> ContentHash;
        [[nodiscard]] auto risk() const noexcept -> Risk;

        // OP:`EffectivePlan`.required_approvals: the approver capabilities the
        // pinned PolicyArtifact ruled must sign this plan before its first Host
        // dispatch, sorted and without repeats. It is a conclusion, not an
        // input: no caller states it, and it is not derived from the risk
        // level -- which humans may approve and whether any must are different
        // facts, and one value cannot answer both if it is a flag.
        [[nodiscard]]
        auto requiredApprovals() const noexcept UF_LIFETIME_BOUND
            -> std::vector<std::string> const&;

        // The Operation this plan is about. Without it a plan is bound only to
        // a registration, so a plan frozen for one Operation could freeze
        // another that happens to be proposed.
        [[nodiscard]]
        auto operationId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto toolName() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto toolVersion() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        // Exact JCS of OP:`EffectivePlan`, the bytes plan_hash covers.
        [[nodiscard]]
        auto canonicalPlan() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto effects() const noexcept UF_LIFETIME_BOUND
            -> std::vector<ProposedEffect> const&;

        [[nodiscard]]
        auto allowedUiActions() const noexcept UF_LIFETIME_BOUND
            -> std::vector<std::string> const&;

        [[nodiscard]] auto limits() const noexcept -> WorkflowLimits;
    };

    // One step of a frozen plan, at one index. The index is inside
    // stepIntentHash, so the same step content at a different position is a
    // different step and cannot be replayed into it.
    class EffectiveStep final
    {
        friend class OperatorPlanAuthority;

        ContentHash m_planHash;
        ContentHash m_stepIntentHash;
        std::string m_operationId;
        std::string m_stepKey;
        std::string m_canonicalStep;
        uint64      m_stepIndex;
        StepKind    m_kind;

        EffectiveStep(
            ContentHash planHash,
            ContentHash stepIntentHash,
            std::string operationId,
            std::string stepKey,
            std::string canonicalStep,
            uint64 stepIndex,
            StepKind kind
        );

    public:
        [[nodiscard]] auto planHash() const -> ContentHash;
        [[nodiscard]] auto stepIntentHash() const -> ContentHash;
        [[nodiscard]] auto stepIndex() const noexcept -> uint64;
        [[nodiscard]] auto kind() const noexcept -> StepKind;

        [[nodiscard]]
        auto operationId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto stepKey() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto canonicalStep() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;
    };

    // Everything the ledger must present to freeze a plan. It is a named type
    // rather than eight parameters because every member is ledger-owned: the
    // command identity, the registration and the two hashes are columns the
    // freezing transaction read out of its own tables, and the proposal is a
    // document only the pinned plugin could have produced.
    //
    // The command identity arrives as bytes rather than as a
    // ValidatedToolInvocation because the invocation is not what the Operation
    // is: a caller that handed one in could hand in a different one from the
    // command the Operation was created for. The `operations` row is the only
    // statement of that command the Operator trusts.
    //
    // The reference member is a call-scoped borrow and nothing stores this
    // aggregate: it is built inside freezePlan, passed to one mint, and
    // destroyed before that call returns. It must never gain a member of its
    // own or be returned.
    struct PlanMintInputs final
    {
        ValidatedDocument const& proposal;

        // The declaration the session's pinned Tool Catalog carries for this
        // tool. It arrives here rather than being remembered from the
        // submission, because the bounds a proposal is judged against belong to
        // the catalog inside project_registration_hash and not to an invocation
        // a caller once presented.
        ToolDescriptor const& descriptor;

        // The capability set of the session this Operation belongs to, read
        // from its own row. The policy's required_controller_capabilities is
        // judged against it, so a controller that could state it could satisfy
        // any rule it liked.
        std::span<std::string const> controllerCapabilities{};

        std::string operationId{};
        std::string toolName{};
        std::string canonicalArgs{};
        ContentHash projectRegistrationHash;
        ContentHash commandFingerprint;
        ContentHash decisionBasisHash;
    };

    // Everything the ledger must present to mint one step. The frozen plan
    // arrives as the exact canonical bytes and hash stored in operation_plans
    // rather than as an EffectivePlan value, because after a freeze the plan
    // lives in that row: a second in-memory copy would be a second spelling of
    // one frozen fact, and only one of them would be what the approval and the
    // audit record are matched against.
    //
    // The reference member is a call-scoped borrow and nothing stores this
    // aggregate: it is built inside mintNextStep, passed to one mint, and
    // destroyed before that call returns.
    struct StepMintInputs final
    {
        ValidatedDocument const& intent;

        // The same catalog declaration mintPlan was given, for the same reason.
        // A step's delivery class and timeout are bounded by what the tool
        // declared about itself, and that statement lives in the catalog.
        ToolDescriptor const& descriptor;

        std::string_view canonicalPlan{};
        std::string      operationId{};
        ContentHash      planHash;
        uint64           stepIndex{};

        // sessions.runtime_artifact_root_hash for the session this Operation
        // belongs to, in the column's own hex spelling. The authority's
        // RuntimeModel binding is matched against it, so a vocabulary parsed
        // from some other artifact of the same registration cannot answer for
        // this session's model. It is the ledger's own row and no caller's.
        std::string_view runtimeArtifactRootHash{};
    };

    // Sole mint for EffectivePlan and EffectiveStep. The two mint functions are
    // private and OperatorCoordinator is their only friend, so no path reaches
    // a mint except through the ledger that owns the decision basis and the
    // command fingerprint.
    class OperatorPlanAuthority final
    {
        friend class OperatorCoordinator;

        ContentHash               m_projectRegistrationHash;
        ContentHash               m_operatorProtocolSchemaHash;
        task::RuntimeModelBinding m_runtimeModel;
        VerifiedPolicyArtifact    m_policy;
        PlanProposalReader        m_readProposal;
        StepIntentReader          m_readStepIntent;

        OperatorPlanAuthority(
            ContentHash projectRegistrationHash,
            ContentHash operatorProtocolSchemaHash,
            task::RuntimeModelBinding runtimeModel,
            VerifiedPolicyArtifact policy,
            PlanProposalReader readProposal,
            StepIntentReader readStepIntent
        );

        [[nodiscard]]
        auto mintPlan(PlanMintInputs const& inputs) const
            -> Result<EffectivePlan>;

        [[nodiscard]]
        auto mintStep(StepMintInputs const& inputs) const
            -> Result<EffectiveStep>;

    public:
        // The exact operator protocol schema bytes are required for the same
        // reason the Journal, Tool Catalog and reconcile owners require theirs:
        // an owner that merely names a hash is a convention. The session
        // manifest supplies the hash to satisfy, and must itself be pinned to
        // this registration.
        //
        // runtimeModel is the same argument one level down. A plan's UI
        // identifiers mean nothing except against the model that would have to
        // resolve them, so the authority is built from the model the Host
        // actually parsed and refuses a manifest pinning a different artifact
        // root. Only TaskHost can mint one, so an authority for a model nobody
        // parsed cannot be constructed at all -- which is what makes the check
        // in mintStep unavoidable rather than optional.
        //
        // The PolicyArtifact is the third argument of the same kind. The
        // Operator evaluates policy rather than accepting a hash a caller
        // supplied, so the authority that mints a plan is built from the exact
        // bytes the manifest's policy_artifact_hash names and can evaluate
        // nothing else.
        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            SessionManifest const& sessionManifest,
            task::RuntimeModelBinding const& runtimeModel,
            std::string_view exactOperatorProtocolSchemaBytes,
            std::string_view exactPolicyArtifactBytes,
            PlanProposalReader readProposal,
            StepIntentReader readStepIntent
        ) -> Result<OperatorPlanAuthority>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;

        // The policy this authority evaluates, so that the ledger can record
        // which artifact ruled a plan without a caller naming one.
        [[nodiscard]] auto policyHash() const -> ContentHash;
    };
}
