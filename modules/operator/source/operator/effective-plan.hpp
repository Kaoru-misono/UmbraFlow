#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"

#include <task/runtime-model-file.hpp>

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace uf::operator_runtime
{
    class OperatorCoordinator;
    class OperatorPlanAuthority;

    // How much one declared effect can cost. It is OP:`Risk` and it is never a
    // request field: a caller that could state its own risk could state
    // read_only for a tool the catalog marked mutating.
    enum class Risk : uint8
    {
        ReadOnly,
        Low,
        Medium,
        High,
        Critical,
    };

    [[nodiscard]] auto riskWireName(Risk risk) noexcept -> std::string_view;

    // The two shapes a workflow step can take: OP:`UIActionIntent` and
    // OP:`WaitIntent`. A wait never reaches Host dispatch, so the kind decides
    // which budget the step spends and whether a dispatch may name it.
    enum class StepKind : uint8
    {
        UiAction,
        Wait,
    };

    [[nodiscard]] auto stepKindWireName(StepKind kind) noexcept -> std::string_view;

    // OP:`WorkflowLimits`. Every member is an upper bound, so clamping is a
    // minimum and a plan can only ever become more restricted.
    struct WorkflowLimits final
    {
        uint32 maximumSteps{};
        uint32 maximumDispatches{};
        uint32 maximumObservations{};
        uint32 maximumWaits{};
        uint64 maximumElapsedMillis{};
    };

    // The ceiling no plugin proposal can exceed. It sits beside the type it
    // bounds so that the clamp and the bound cannot drift into two files. The
    // five numbers are an Operator choice, not a product one: nothing in the
    // frozen bundle names a ceiling, and a plan is allowed to be smaller.
    inline constexpr auto k_workflowCeiling = WorkflowLimits{
        .maximumSteps        = 64U,
        .maximumDispatches   = 64U,
        .maximumObservations = 256U,
        .maximumWaits        = 64U,
        .maximumElapsedMillis = 600'000U,
    };

    // One OP:`ExpectedEffect` in the terms the Operator acts on. The project
    // payload stays opaque: it is carried so that the minted plan is the exact
    // document the checked-in schema defines, and it is never interpreted.
    struct ProposedEffect final
    {
        std::string namespacedType{};

        // Critical is the default for the same reason ToolMutability defaults
        // to Mutating: an effect whose risk failed to parse must be treated as
        // the most restricted of the five, never the least.
        Risk        risk{Risk::Critical};
        std::string scopeKind{};
        std::string scopeKey{};
        ContentHash payloadSchemaHash;
        std::string opaqueProjectPayload{};
    };

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
    struct StepIntentClaims final
    {
        std::string stepKey{};
        std::string surfaceId{};
        std::string uiTargetId{};
        std::string actionId{};
        StepKind    kind{StepKind::Wait};
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
        WorkflowLimits              m_limits;
        Risk                        m_risk;
        bool                        m_approvalRequired;

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
            WorkflowLimits limits,
            Risk risk,
            bool approvalRequired
        );

    public:
        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto operatorProtocolSchemaHash() const -> ContentHash;
        [[nodiscard]] auto commandFingerprint() const -> ContentHash;
        [[nodiscard]] auto decisionBasisHash() const -> ContentHash;
        [[nodiscard]] auto effectEnvelopeHash() const -> ContentHash;
        [[nodiscard]] auto planHash() const -> ContentHash;
        [[nodiscard]] auto risk() const noexcept -> Risk;

        // Whether the derived risk requires a human approval before the first
        // Host dispatch. It is a conclusion, not an input: no caller states it.
        [[nodiscard]] auto approvalRequired() const noexcept -> bool;

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
        std::string operationId{};
        std::string toolName{};
        std::string toolVersion{};
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
        PlanProposalReader        m_readProposal;
        StepIntentReader          m_readStepIntent;

        OperatorPlanAuthority(
            ContentHash projectRegistrationHash,
            ContentHash operatorProtocolSchemaHash,
            task::RuntimeModelBinding runtimeModel,
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
        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            SessionManifest const& sessionManifest,
            task::RuntimeModelBinding const& runtimeModel,
            std::string_view exactOperatorProtocolSchemaBytes,
            PlanProposalReader readProposal,
            StepIntentReader readStepIntent
        ) -> Result<OperatorPlanAuthority>;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
    };
}
