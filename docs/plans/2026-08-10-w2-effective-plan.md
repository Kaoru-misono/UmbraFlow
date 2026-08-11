# W2: EffectivePlan authority

Status: **landed 2026-08-11 in `848e390`**, with deviations. Everything below is
the pre-landing specification and is left as written.
Date: 2026-08-10 (landed 2026-08-11)
Scope: `umbraflow-cpp` only. No consumer-project writes.
Closes: `c05`, `c08`, `s04`
Depends on: nothing. `W3`, `W4` and `W6` depend on this.

Authority above this document: [`2026-08-09-runtime-hardening-rewrite.md`](2026-08-09-runtime-hardening-rewrite.md).
Ordering and requirement state: [`2026-08-10-next-block.md`](2026-08-10-next-block.md) §2 and §4.

> **Amended 2026-08-10: read §1.2 of
> [the W2-W7 reconciliation](2026-08-10-w2-w7-reconciliation.md) first.** Every
> `contract-*` gate name this document treats as existing was renamed to
> `schema-*` by `dcc43b5`; `ctest -N` lists no `contract-control-c05`,
> `contract-control-c08` or `contract-state-s04`. This item owes **new**
> `contract-*` cases — the migration report updated first, then
> `tests/CMakeLists.txt`, then a suite's `CASES` list — not a rewrite of cases
> that no longer exist under those names.

> **Landed 2026-08-11 in `848e390`. Read what follows as the plan, not as the
> tree.** `c05`, `c08` and `s04` are closed; all three `contract-*` cases were
> written new, though the migration report was updated after the registration
> rather than before it and had to be repaired. What differs from this document
> is recorded once, in `848e390`'s message, and is not back-written here:
> `PlanMintInputs` takes the command identity as bytes from the `operations`
> row rather than a `ValidatedToolInvocation`, `StepMintInputs` takes the
> stored canonical plan rather than an in-memory `EffectivePlan`, and
> `ProposedEffect` keeps `opaque_project_payload`. §6's fingerprint and table
> list are superseded twice over — by the landing and by the removal of
> `runtime_publications` in the same commit; at `848e390` the tree carried
> `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`
> over 20 tables. (Superseded three landings later on the same day, and once
> more after that: the tree now carries
> `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`
> over 23 tables — `bda31e4b18…` stood between W7 and `07abc3e`, which renamed
> eight DDL columns to `controlled_target_id`, and `be80aca714…` stood between
> that and this block, which renamed four more to the journal record schema's
> member names. Nothing else in this note moves with it.) *(Corrected
> 2026-08-11: this read `be80aca714…` as current. See
> [journal record binding](2026-08-11-journal-record-binding.md).)*
>
> **All 15 mutations in §9 were run — the first time any of this block's 23
> were.** Eleven turn their case red. `T4` is not applicable as written, and
> the reason is a stronger guarantee than it asked for: `DecisionBasisParts`
> carries the four content hashes and nothing else, so there is no counter,
> lease, epoch or token in scope to fold in. **`T2`, `T10` and `T13` stay
> green**, and they are defects in the cases rather than in the code:
>
> - `T2` — the registration guard in `mintPlan` is not the guard doing the
>   work; `freezePlan` has already verified the plugin against the session and
>   refuses first. Kept as defence in depth, recorded as not load-bearing.
> - `T10` — "a plan freezes once" is enforced by the state machine refusing a
>   non-`Proposed` Operation, not by `operation_plans`' primary key. This
>   document's own argument against double enforcement (§5) applies to the key.
> - `T13` — **the audit row has no reader.** Corrupting
>   `authority_decisions.decision_basis_hash` is invisible to every test,
>   because `reserveDispatch` reads the basis from `operation_plans` and
>   nothing on the public surface reads `authority_decisions` back.
>
> `T11` was green until the fixture was fixed: the read-only tool had no plan,
> so removing the Operator's refusal moved the failure into the plugin. The
> fixture was proving the property, not the code.
>
> **What this document still owes, and where.** Under `CLAUDE.md`'s archiving
> precondition it cannot be archived while these stand. The three green
> mutations and §10's three unenforced limits are carried in
> [the next block](2026-08-10-next-block.md) §2, beside the requirements they
> qualify; `T13` is additionally recorded in
> [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md), because
> an audit column with no reader is a repository-wide shape rather than a fact
> about `c05`. §11's open questions 4 and 5 are still open and are now the
> reason `StepKind::Wait` is unexercised.

Every file-level claim below was read out of the tree at commit
`design/annotation-system-v2` on 2026-08-10. The tree is being edited by other
agents while this was written: `tests/operator/test-control-contract.cpp` and
`tests/operator/test-ledger.cpp` both changed size during the reading pass, and
behavioural cases moved between files. Line numbers are therefore given as
anchors, not as addresses; re-run the greps in §8 before editing. Signatures,
DDL and the schema fingerprint were re-read after the last observed change and
are current.

## 1. The authority boundary

Today nothing mints a plan. `ProjectPluginHandle::plan` and
`ProjectPluginHandle::nextStep` exist
(`modules/operator/source/operator/project-plugin.hpp:190,193`) and are called
only from `tests/operator/test-project-plugin-contract.cpp`. The ledger never
calls either. `OperatorCoordinator::reserveDispatch` instead accepts three
finished hashes from whoever calls it:

```cpp
auto reserveDispatch(
    std::string const& operationId,
    uint64 expectedRevision,
    ControlLease const& lease,
    ContentHash const& decisionBasisHash,
    ContentHash const& frozenPlanHash,
    ContentHash const& stepIntentHash,
    std::string const& authorityDecisionId,
    std::optional<ApprovalGrant> const& approval
) -> Result<DispatchReservation>;
```

Those three hashes are written verbatim into `authority_decisions` and
`dispatches` and matched against `approvals`
(`modules/operator/source/operator/ledger.cpp:3108,3147-3154,3178,3208`). A
caller that can pick them can make the audit record say anything, can make an
approval issued for one step authorise another by reusing its hash, and can
freeze a plan that corresponds to no plugin output at all.

### What the plugin proposes

`plugin.plan(input)` returns a document the Operator reads as
`#/$defs/PlanProposal` in `schema/umbraflow-operator-v1.schema.json:483-509`.
Its complete member list is checked in:

```json
"PlanProposal": {
  "required": [
    "tool_name", "tool_version", "canonical_args",
    "effects", "allowed_ui_actions", "workflow_limits"
  ]
}
```

`plugin.next_step(input)` returns `#/$defs/UIActionIntent` (line 562) or
`#/$defs/WaitIntent` (line 603). Both carry a `step_key`.

### What the Operator mints

`#/$defs/EffectivePlan` (lines 510-552) has twelve members. Six are echoed from
the proposal after they have been checked against the Operation; six the
Operator derives and the plugin cannot name:

| EffectivePlan member | Origin |
|---|---|
| `tool_name`, `tool_version`, `canonical_args` | echoed, but must equal the `ValidatedToolInvocation` the Tool Catalog owner minted; a mismatch refuses the plan |
| `effective_effects` | echoed from `effects`, sorted and de-duplicated |
| `allowed_ui_actions` | echoed, de-duplicated |
| `workflow_limits` | echoed, then clamped down to the Operator ceiling; never widened |
| `command_fingerprint` | **derived** — already computed and stored by `createOrLoadOperation` (`ledger.cpp:2613-2625`, stored in `operations.command_fingerprint`) |
| `project_registration_hash` | **derived** — the plan authority's own binding, cross-checked against the session's pinned registration |
| `decision_basis_hash` | **derived** — read from the snapshot row the Operation names (§4) |
| `risk` | **derived** — the maximum of `effective_effects[].risk` |
| `required_approvals` | **derived** — from `risk` through an Operator-owned table (§10 records the gap) |
| `plan_hash` | **derived** — sha256 over the canonical plan with `plan_hash` itself omitted |

### What a lying plugin gains, and what stops it

| Lie | Containment |
|---|---|
| Proposes a different `tool_name` / `tool_version` / `canonical_args` than the caller invoked | The mint compares all three against the `ValidatedToolInvocation` and refuses. The plugin cannot substitute a tool. |
| Proposes `workflow_limits` larger than the ceiling | Each limit is `min(proposed, k_workflowCeiling.*)`. Widening is arithmetically impossible, not policy-checked. |
| Proposes a `PlanProposal` a second time to replace a frozen plan | `operation_plans` is keyed by `operation_id`; the second insert is a primary-key violation. There is no update path. |
| Returns a `next_step` whose `step_key` is not in the frozen plan's `allowed_ui_actions` | The step mint refuses. The allowed set is fixed at freeze time. |
| Returns the same `next_step` document forever | Each mint takes the next index, the index is inside the step identity, and the step budget is consumed. The workflow stops at the bound (§5). |
| **Under-declares its own effects, so `risk` comes out lower than the truth** | **Nothing in W2 prevents this.** The plugin is the only authority on what its own tools do. Containment is attribution, not prevention: the plugin bytes are content-addressed by `plugin_hash`, which participates in `project_registration_hash`, which pins the session. A plugin that lies is a different registration and a different audit trail. The one mechanism that would catch a *new* effect type is `unknown_effect_decision: "deny"` in `schema/umbraflow-policy-v1.schema.json`, and no code evaluates a policy artifact today (§10). |
| Proposes `allowed_ui_actions` naming actions the runtime model does not contain | Nothing refuses it, and nothing needs to: an `allowed_ui_actions` entry is a `step_key`, matched against the step intent's own `step_key`, and it is not a RuntimeModel identifier. |
| Returns a `next_step` whose `surface_id`, `ui_target_id` or `action_id` the installed RuntimeModel does not define | The step mint refuses. `OperatorPlanAuthority` is built from the `task::RuntimeModelBinding` the Host parsed out of the artifact the session pinned, and `mintStep` asks whether each of the three names is in the vocabulary that binding publishes. It reads no RuntimeModel field: the trusted Luau compiler publishes three flat lists of opaque identifiers, and the Operator does membership on them. |

The boundary in one sentence: the plugin describes, the Operator decides, and
every field the Operator decides is computed from bytes the ledger already
holds.

An ordinary caller gains nothing new. It submits a tool and arguments to
`createOrLoadOperation` as before. It never sees a `PlanProposal`, never
constructs an `EffectivePlan`, and after W2 can no longer name a plan hash, a
step hash, a decision basis, or an approval requirement.

## 2. What is already in the checked-in contract

Verified by reading `schema/umbraflow-operator-v1.schema.json`. Nothing in
this list needs to be added:

- `PlanProposal` (483), `EffectivePlan` (510), `PlanVersion` (689).
- `WorkflowLimits` (427) with `maximum_steps`, `maximum_dispatches`,
  `maximum_observations`, `maximum_waits`, `maximum_elapsed_ms`.
- `WorkflowBudget` (965) with the four `remaining_*` counters.
- `UIActionIntent` (562) and `WaitIntent` (603), both keyed by `step_key`.
- `ExpectedEffect` (445) with `risk`, `scope_kind`, `scope_key`.
- `Risk` (26): `read_only`, `low`, `medium`, `high`, `critical`.
- `DecisionBasis` (363) — the exact four inputs plus the derived hash.
- `SnapshotParts` (281) and `SnapshotIdentity` (319).
- `Operation` (985) already requires `current_step` and `workflow_budget`.

**The schema must not change.** Its members come from the frozen v1.9 bundle,
and the authority document is explicit: "Changing any product field,
disposition or ownership still requires a new consumer bundle root." W2 makes
the existing definitions executable and adds nothing to them.

The three gates it closes are today schema-shape assertions only:
`tests/operator/test-control-contract.cpp:297` (`contract-control-c05`),
`:336` (`contract-control-c08`), and
`tests/operator/test-state-contract.cpp:239` (`contract-state-s04`). Each reads
the schema file and greps for member names. Their bodies are replaced by §9;
the schema assertions stay, appended to the behavioural ones, so W10 has
nothing left to rename for these three.

## 3. Header-level declarations

New file `modules/operator/source/operator/effective-plan.hpp`. Module sources
are globbed (`cmake/build.cmake:118`), so no CMake edit is needed.

The pattern is the one `ValidatedToolInvocation` and `ValidatedReconcileOutcome`
already use: a value type whose constructor is private, with exactly one friend,
and that friend is an authority created from a `VerifiedProjectRegistration`
plus the exact bytes of the schema the registration pins.

The binding here is the *operator protocol* schema, not a project schema. The
`ProjectRegistrationManifest` field list is frozen by the authority document §2
and contains no plan-schema hash, so a new one cannot be added. `SessionManifest`
already carries `operator_protocol_schema_hash`
(`modules/operator/source/operator/manifest.hpp:135`), and `PlanProposal` lives
in that schema. So the plan authority binds to a registration *and* a session
manifest, and verifies both.

```cpp
#pragma once

#include "manifest.hpp"
#include "project-plugin.hpp"
#include "tool-invocation.hpp"

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

    // The two shapes a workflow step can take: OP:`UIActionIntent` and
    // OP:`WaitIntent`. A wait never reaches Host dispatch, so the kind decides
    // which budget the step spends and whether a dispatch may name it.
    enum class StepKind : uint8
    {
        UiAction,
        Wait,
    };

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
    // bounds so that the clamp and the bound cannot drift into two files.
    inline constexpr auto k_workflowCeiling = WorkflowLimits{
        .maximumSteps          = 64U,
        .maximumDispatches     = 64U,
        .maximumObservations   = 256U,
        .maximumWaits          = 64U,
        .maximumElapsedMillis  = 600'000U,
    };

    // One OP:`ExpectedEffect` in the terms the Operator acts on. The project
    // payload stays opaque; these five fields are the only ones core reads.
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
    struct StepIntentClaims final
    {
        std::string stepKey{};
        StepKind    kind{StepKind::Wait};
    };

    // Trusted deployment callbacks. Each must validate the complete operator
    // protocol definition -- PlanProposal, UIActionIntent or WaitIntent -- and
    // reject anything that is not exact RFC 8785 JCS. Neither is passed to
    // plugin code or published in a business VM.
    using PlanProposalReader = std::function<
        Result<PlanProposalClaims>(std::string_view exactProposalJcs)
    >;
    using StepIntentReader = std::function<
        Result<StepIntentClaims>(std::string_view exactStepJcs)
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

        // The Operation this plan is about. Without it a plan is bound only to
        // a registration, so a plan frozen for one Operation could freeze
        // another that happens to be proposed -- the same reach ValidatedReconcile-
        // Outcome::operationId exists to cut off, one step earlier.
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
    // rather than five parameters because every member is ledger-owned: a
    // caller cannot construct one without already holding a validated
    // invocation, a plugin output, and hashes the ledger read from its own
    // tables.
    //
    // The two reference members are call-scoped borrows and nothing stores
    // this aggregate: it is built inside freezePlan, passed to one mint, and
    // destroyed before that call returns. It must never gain a member of its
    // own or be returned.
    struct PlanMintInputs final
    {
        ValidatedToolInvocation const& invocation;
        ValidatedDocument const&       proposal;
        std::string                    operationId{};
        ContentHash                    commandFingerprint;
        ContentHash                    decisionBasisHash;
    };

    struct StepMintInputs final
    {
        EffectivePlan const&     plan;
        ValidatedDocument const& intent;
        uint64                   stepIndex{};
    };

    // Sole mint for EffectivePlan and EffectiveStep. The two mint functions are
    // private and OperatorCoordinator is their only friend, so no path reaches
    // a mint except through the ledger that owns the decision basis and the
    // command fingerprint. Ruling 2026-08-10 §4 -- "a second trusted object is
    // a second place authority can leak from" -- is why this object cannot be
    // driven from outside the ledger.
    class OperatorPlanAuthority final
    {
        friend class OperatorCoordinator;

        ContentHash        m_projectRegistrationHash;
        ContentHash        m_operatorProtocolSchemaHash;
        PlanProposalReader m_readProposal;
        StepIntentReader   m_readStepIntent;

        OperatorPlanAuthority(
            ContentHash projectRegistrationHash,
            ContentHash operatorProtocolSchemaHash,
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
        [[nodiscard]]
        static auto create(
            VerifiedProjectRegistration const& registration,
            SessionManifest const& sessionManifest,
            std::string_view exactOperatorProtocolSchemaBytes,
            PlanProposalReader readProposal,
            StepIntentReader readStepIntent
        ) -> Result<OperatorPlanAuthority>;
    };
}
```

Derivations, all in the anonymous namespace of `effective-plan.cpp`, following
the hand-written JCS emitter already in
`modules/operator/source/operator/manifest.cpp:90-113`:

- `effect_envelope_hash` = sha256 of the JCS array of `effective_effects`. The
  array is sorted by `(namespaced_type, scope_kind, scope_key)` UTF-8 bytes and
  an exact duplicate of that triple is refused, mirroring the artifact-root rule
  in the frozen authority ("non-empty, unique, and sorted by UTF-8 bytes"). JCS
  does not sort arrays, so without this the same effect set in two orders would
  produce two envelopes.
- `plan_hash` = sha256 of the canonical `EffectivePlan` object **with the
  `plan_hash` member omitted**. `plan_hash` is a member of the definition and
  cannot cover itself. The stored `canonical_plan` then carries all twelve
  members, `plan_hash` included.
- `step_intent_hash` = sha256 of
  `plan_hash.hex()` `0x00` `decimal(step_index)` `0x00` `canonical step JCS`.
  Because `plan_hash` already covers the command fingerprint and the decision
  basis, a step is bound to the whole world the plan was frozen against.

## 4. `s04`: the decision basis becomes derived

The checked-in contract fixes the input list exactly
(`schema/umbraflow-operator-v1.schema.json:363-380`):

```json
"DecisionBasis": {
  "required": [
    "state_resolution_hash",
    "project_observation_hash",
    "project_state_hash",
    "session_manifest_hash",
    "decision_basis_hash"
  ]
}
```

Four inputs, and the fifth member is the result. So:

`decision_basis_hash = sha256(JCS of the four hashes, keys sorted, decision_basis_hash omitted)`

### Where the four come from

`SnapshotParts` (line 281) is the record that carries them, and it also carries
`decision_basis_hash` — the basis is derived once, when the snapshot is created,
and every plan frozen against that snapshot inherits it. This is what makes the
basis a property of the observed world rather than of the request.

Of the fourteen `SnapshotParts` members other than the derived hash, eight are
already the Operator's and must not be accepted from anyone:

| Member | Operator source |
|---|---|
| `session_epoch`, `lease_id`, `fencing_token` | the verified `ControlLease`, already re-read from `control_leases` inside `createSnapshot` (`ledger.cpp:2535-2571`) |
| `controlled_target_id`, `project_instance_key` | the `sessions` row |
| `session_manifest_hash` | `sessions.manifest_hash` |
| `project_state_revision`, `project_state_hash` | the `project_state` row |

Six come from the observation and are the only ones a caller supplies after W2:

```cpp
    // What one observation contributes to a snapshot. Everything else in
    // OP:`SnapshotParts` the Operator reads from its own tables, so a caller
    // cannot pin a snapshot to a world the ledger does not hold. W3 removes
    // the two hashes below as well, by composing the UI observation and
    // plugin.derive itself.
    struct ObservedSnapshotParts final
    {
        std::string observationId{};
        ContentHash stateResolutionHash;
        ContentHash projectObservationHash;
        uint64      targetGeneration{};
        uint64      projectObservationRevision{};
        uint64      availabilityRevision{};
    };
```

Two of the four decision-basis inputs (`project_state_hash`,
`session_manifest_hash`) therefore come from the database immediately. The other
two come from the observer until W3 composes them. What W2 closes is that **no
caller names the resulting hash**, and no caller names it at plan or dispatch
time at all. What W3 closes is the remaining two components.

This is a correction to `2026-08-10-next-block.md` §2, which assigns "derive the
snapshot identity instead of accepting one" to W3 while assigning `s04` to W2.
`s04` cannot be closed without a snapshot that carries the four components, so
the split is: **W2 derives `identity_hash` and `decision_basis_hash` from the
parts; W3 derives the parts.** This is inferred from the two documents plus the
schema, not stated in either; it is the first thing to confirm before starting.

### What must NOT enter the decision basis

Getting this list wrong is the requirement. `SnapshotIdentity` has eighteen
members and `DecisionBasis` has four; the difference is the whole point. The
identity hash answers "is this the same snapshot"; the decision basis answers
"is this the same decision input". Everything below is in the first and must
stay out of the second:

| Excluded | Why |
|---|---|
| `lease_id`, `fencing_token`, `session_epoch`, `session_id`, `controller_id` | authority and liveness, not content. The same world observed after a lease takeover is the same decision input; a plan frozen against it is still about the same world. Including them would make every fence bump invalidate an identical basis. |
| `token_id` (the opaque snapshot token) | a transport handle. `s03` already rules it "an opaque CAS reference, not a permission". |
| `snapshot_revision`, `project_state_revision`, `project_observation_revision`, `availability_revision`, `target_generation` | counters. Two states with equal content and different revisions are the same decision input; `project_state_hash` already carries the content. |
| `observation_id`, `controlled_target_id`, `project_instance_key` | identifiers of where the observation came from, not of what it said. |
| wall-clock values: `created_at`, `updated_at`, `plan_frozen_at`, `dispatch_started_at`, `expires_at`, `deadline` | non-deterministic. Two identical decisions one second apart must hash identically or the basis proves nothing. |
| `client_request_id`, `correlation_id`, `idempotency_namespace`, `receipt_id` | request and transport identity. `client_request_id` belongs to idempotency, which `operations` already keys on. |
| `dispatch_sequence`, `step_index`, retry or attempt counters | progress within one decision, not the decision. Step identity carries the index separately (§3). |
| `tool_name`, `tool_version`, `canonical_args` | these are the *command*, not the *basis*. They are covered by `command_fingerprint`, which `EffectivePlan` carries as its own member; folding them in would make one hash mean two things and make "the same world, a different command" indistinguishable from "a different world". |
| `policy_artifact_hash` | not directly. It is already inside `session_manifest_hash` (`manifest.hpp:135`, `manifest.cpp:103`), so a policy change does move the basis — transitively and once, rather than twice. |
| `plugin_hash`, `project_registration_hash` | same reasoning: both are inside `session_manifest_hash` through `project_registration_hash`. Adding them directly would double-count. |

## 5. `c08`: bounded multi-step workflow

"Bounded multi-step workflow" means: one Operation may run more than one step
against one frozen plan; the number of steps and the number of Host dispatches
are fixed when the plan freezes; a step cannot be replayed, reordered, or
skipped; and running out of budget can never terminate the Operation, only stop
it from doing more.

### The sequence

1. `createOrLoadOperation` — Proposed. Unchanged.
2. `freezePlan` — calls `plugin.plan`, mints the `EffectivePlan`, writes
   `operation_plans`, and transitions the Operation itself to `Ready` or
   `AwaitingApproval` according to the derived `required_approvals`. The caller
   no longer chooses which.
3. `mintNextStep` — calls `plugin.next_step`, mints the `EffectiveStep` at index
   `max(step_index) + 1`, writes `operation_steps`.
4. `reserveDispatch` — finds the one pending UI-action step, writes
   `authority_decisions` and `dispatches`, links the step to the dispatch, and
   moves the Operation to `Running`.
5. `recordDeliveryOutcome` — `Reconciling`. Unchanged.
6. `commitReconciliation` with a `Continue` disposition — stays `Reconciling`.
   Unchanged.
7. Back to 3 for the next step.

The state machine already has the edges for this and needs no change:
`Reconciling --NextStepReady--> Running` and
`Reconciling --NextStepApprovalRequired--> AwaitingApproval`, both under
`FrozenGuard::Frozen` (`operation.cpp:59-61`). After W2 those two events are
driven by `mintNextStep`, not by the caller.

### Where the counter lives

In SQLite, as `MAX(step_index)` over `operation_steps` for the Operation, read
inside the same `BEGIN IMMEDIATE` transaction that inserts the next row. There
is no in-memory counter: `OperatorCoordinator::Impl` holds only a database
handle, two paths and the session epoch (`ledger.cpp:1049-1055`), and a counter
that lives in a process cannot survive the restart-epoch path `c02` exists for.

### What bounds it

`operation_plans.maximum_steps` and `operation_plans.maximum_dispatches`, both
written at freeze time from the clamped `WorkflowLimits`. `mintNextStep` refuses
when `step_index > maximum_steps`. `reserveDispatch` refuses when the existing
dispatch count is already `maximum_dispatches`. Two counters because a wait step
consumes a step and no dispatch.

### What happens at the bound

`mintNextStep` returns `fail(AutomationErrorKind::ActionRejected, ...)`. The
Operation stays exactly where it was — `Reconciling`, plan frozen, mutation
chain still held. It does not transition to a terminal state and it does not
release the chain. That is the same fail-closed shape as the post-dispatch edge
in the frozen authority §1: only reconciliation may establish a business
terminal disposition. A caller that wants to give up drives
`OperationEvent::PostDispatchAbort`, which the machine already routes from
`Running` and `AwaitingApproval` to `Reconciling` (`operation.cpp:68-69`).

### How a step's identity resists replay and reorder

`step_intent_hash` covers `plan_hash`, the decimal `step_index`, and the exact
step JCS (§3). Three consequences:

- the same step document at index 2 has a different identity than at index 1, so
  a replay of step 1's approval cannot authorise step 2 — the approval row is
  matched on `step_intent_hash` (`ledger.cpp:3128`);
- a step minted against one frozen plan cannot be presented under another,
  because `plan_hash` is in the preimage;
- indices are dense and monotone because they come from `MAX(step_index) + 1`
  inside the transaction, so there is no gap to slip a step into.

Only one UI-action step may be pending dispatch at a time. `mintNextStep`
refuses if a UI-action row with `dispatch_sequence IS NULL` already exists.
**This check is deliberately not also expressed as a partial unique index.** A
second enforcement would keep the test in §9 green after the first was deleted,
which is exactly the class of test this repository treats as a defect. The check
runs inside `BEGIN IMMEDIATE` with the existing `operations.revision` CAS, so
one enforcement is enough.

### Not enforced by W2

`maximum_observations`, `maximum_waits` and `maximum_elapsed_ms` are stored in
the frozen plan and are not enforced. Observations are W3's and the Agent
budgets are W7's; elapsed time has no Operation start timestamp in the ledger
today, and the deadline path (`OperationEvent::DeadlineExpired`) already covers
wall-clock termination. Storing them without enforcing them is stated here so
that no later reader mistakes the column for a gate.

## 6. Storage, and the schema fingerprint

Two new tables and two new columns. Insert them into the `R"sql(...)"` block in
`OperatorCoordinator::open`'s `initialize` (`ledger.cpp:502-768`), keeping the
existing formatting exactly — see the fingerprint warning below.

`snapshots` gains two columns (existing definition at `ledger.cpp:625-631`):

```sql
                    CREATE TABLE IF NOT EXISTS snapshots(
                        token TEXT PRIMARY KEY,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        identity_hash TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        canonical_parts TEXT NOT NULL,
                        lease_revision INTEGER NOT NULL CHECK(lease_revision > 0)
                    ) STRICT;
```

`canonical_parts` holds the exact `SnapshotParts` JCS, following
`journal_events.opaque_project_payload` and
`project_state.canonical_opaque_payload`. Both hashes are recomputable from
it, which is what lets §9 falsify the derivation.

```sql
                    CREATE TABLE IF NOT EXISTS operation_plans(
                        operation_id TEXT PRIMARY KEY
                            REFERENCES operations(operation_id),
                        plan_hash TEXT NOT NULL,
                        command_fingerprint TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        effect_envelope_hash TEXT NOT NULL,
                        project_registration_hash TEXT NOT NULL
                            REFERENCES project_registrations(registration_hash),
                        risk TEXT NOT NULL CHECK(risk IN (
                            'read_only', 'low', 'medium', 'high', 'critical'
                        )),
                        required_approvals INTEGER NOT NULL
                            CHECK(required_approvals IN (0, 1)),
                        maximum_steps INTEGER NOT NULL CHECK(maximum_steps > 0),
                        maximum_dispatches INTEGER NOT NULL
                            CHECK(maximum_dispatches > 0),
                        maximum_observations INTEGER NOT NULL
                            CHECK(maximum_observations > 0),
                        maximum_waits INTEGER NOT NULL CHECK(maximum_waits >= 0),
                        maximum_elapsed_ms INTEGER NOT NULL
                            CHECK(maximum_elapsed_ms > 0),
                        canonical_plan TEXT NOT NULL
                    ) STRICT;

                    CREATE TABLE IF NOT EXISTS operation_steps(
                        operation_id TEXT NOT NULL
                            REFERENCES operation_plans(operation_id),
                        step_index INTEGER NOT NULL CHECK(step_index > 0),
                        step_kind TEXT NOT NULL
                            CHECK(step_kind IN ('ui_action', 'wait')),
                        step_key TEXT NOT NULL,
                        step_intent_hash TEXT NOT NULL,
                        canonical_step TEXT NOT NULL,
                        dispatch_sequence INTEGER,
                        PRIMARY KEY(operation_id, step_index),
                        FOREIGN KEY(operation_id, dispatch_sequence)
                            REFERENCES dispatches(operation_id, dispatch_sequence)
                    ) STRICT;
```

`operation_plans.operation_id` is the primary key, so a plan freezes once and a
second `freezePlan` is a constraint violation rather than a policy check.
`required_approvals` is stored as 0/1 rather than as the schema's array because
the only approval kind the ledger has is the single human `ApprovalToken` in
`approvals`; a list would be a column nothing reads.

Write order inside `reserveDispatch`'s transaction, so the composite foreign key
holds: read the pending step, insert `authority_decisions`, insert `dispatches`,
`UPDATE operation_steps SET dispatch_sequence=?`, update `operations`.

### The fingerprint, which is verified at open

`verifyExactDatabaseSchema` (`ledger.cpp:336-382`) canonicalises every
`sqlite_schema` row as `length:value` over four columns ordered by `(type,
name)`, hashes it, and compares against a literal at **`ledger.cpp:372`**:

```cpp
                    "sha256:5738e6f98534efbdfc3114413de70c032b64e2cbaa84d4c152ec6cbb512120a4"
```

That literal must be recomputed. So must the table list at `ledger.cpp:476-482`,
which gains `operation_plans` and `operation_steps` in sorted position:

```cpp
                constexpr auto expectedTables = std::string_view{
                    "approvals,authority_decisions,control_leases,control_transitions,"
                    "dispatches,fencing_high_water,journal_events,operation_plans,"
                    "operation_steps,operations,project_instances,project_registrations,"
                    "project_state,reconciliations,runtime_artifacts,runtime_installations,"
                    "runtime_publications,runtime_state,sessions,snapshots"
                };
```

`PRAGMA user_version` stays `1`. Bumping it would imply two supported shapes;
the exact-bytes check already refuses anything else, and nothing is released.

Recompute the literal from a freshly created database rather than by hand — the
canonical string is the *stored* DDL text, comments and indentation included, so
reformatting the `R"sql(...)"` block changes the fingerprint even when the schema
is identical:

```python
import hashlib, sqlite3
db = sqlite3.connect('fresh-operator-runtime.sqlite')
rows = db.execute(
    "SELECT type, name, tbl_name, coalesce(sql, '') FROM sqlite_schema "
    "WHERE name NOT LIKE 'sqlite_%' ORDER BY type, name"
).fetchall()
canonical = ''.join(
    f'{len(v.encode("utf-8"))}:{v}' for row in rows for v in row
)
print('sha256:' + hashlib.sha256(canonical.encode('utf-8')).hexdigest())
```

Consequence to state in the commit message: any existing
`operator-runtime.sqlite` stops opening, with "Operator database schema bytes do
not match exact v1". That is the intended break. The migration is to delete the
file; there is no reader for the old shape and none may be added.

## 7. Signature changes

### `OperatorCoordinator::createSnapshot`

```cpp
// before
auto createSnapshot(
    ControlLease const& lease,
    ContentHash const& identityHash
) -> Result<SnapshotRecord>;

// after
auto createSnapshot(
    ControlLease const& lease,
    ObservedSnapshotParts const& observed
) -> Result<SnapshotRecord>;
```

`SnapshotRecord` gains `ContentHash decisionBasisHash;`. `identityHash` stays a
member but is now derived, not accepted.

### `OperatorCoordinator::freezePlan` — new

```cpp
// The plan is minted here rather than by the caller because the command
// fingerprint and the decision basis are the ledger's: they come from the
// operations row and the snapshot row, and a plan that named its own would be
// a plan about a world nobody observed.
[[nodiscard]]
auto freezePlan(
    std::string const& operationId,
    uint64 expectedRevision,
    ControlLease const& lease,
    ProjectPluginHandle const& plugin,
    OperatorPlanAuthority const& planAuthority
) -> Result<FrozenPlan>;
```

```cpp
struct FrozenPlan final
{
    StoredOperation operation;
    ContentHash     planHash;
    ContentHash     decisionBasisHash;
    ContentHash     effectEnvelopeHash;
    Risk            risk{Risk::Critical};
    WorkflowLimits  limits{};
    bool            approvalRequired{};
};
```

### `OperatorCoordinator::mintNextStep` — new

```cpp
[[nodiscard]]
auto mintNextStep(
    std::string const& operationId,
    uint64 expectedRevision,
    ControlLease const& lease,
    ProjectPluginHandle const& plugin,
    OperatorPlanAuthority const& planAuthority
) -> Result<PlannedStep>;
```

```cpp
struct PlannedStep final
{
    StoredOperation operation;
    ContentHash     stepIntentHash;
    std::string     stepKey{};
    uint64          stepIndex{};
    StepKind        kind{StepKind::Wait};
};
```

### `OperatorCoordinator::reserveDispatch`

```cpp
// before
auto reserveDispatch(
    std::string const& operationId,
    uint64 expectedRevision,
    ControlLease const& lease,
    ContentHash const& decisionBasisHash,
    ContentHash const& frozenPlanHash,
    ContentHash const& stepIntentHash,
    std::string const& authorityDecisionId,
    std::optional<ApprovalGrant> const& approval
) -> Result<DispatchReservation>;

// after
auto reserveDispatch(
    std::string const& operationId,
    uint64 expectedRevision,
    ControlLease const& lease,
    std::string const& authorityDecisionId,
    std::optional<ApprovalGrant> const& approval
) -> Result<DispatchReservation>;
```

The three hashes are gone, and no step index replaces them. The Operation has at
most one pending UI-action step, so the dispatch names nothing — it finds that
step, or it fails. The alternative, passing a `stepIndex` or a `PlannedStep`
value, was rejected: an index is a caller-chosen selector over rows the caller
did not write, and a value handed back in can be a value minted for a different
Operation.

`DispatchReservation` grows what the caller now needs and can no longer invent:

```cpp
struct DispatchReservation final
{
    ContentHash frozenPlanHash;
    ContentHash decisionBasisHash;
    ContentHash stepIntentHash;
    uint64      dispatchSequence{};
    uint64      operationRevision{};
    uint64      stepIndex{};
};
```

### `OperatorCoordinator::issueApproval`

`ApprovalRequest` loses four caller fields; they are read from `operation_plans`
and the pending `operation_steps` row:

```cpp
// before
struct ApprovalRequest final
{
    std::string  operationId{};
    ControlLease lease;
    ContentHash  frozenPlanHash;
    ContentHash  stepIntentHash;
    ContentHash  decisionBasisHash;
    ContentHash  effectEnvelopeHash;
    ContentHash  policyHash;
    std::string  approverPrincipal{};
    ContentHash  approverCapabilityHash;
    uint64       expiresAtUnixMillis{};
};

// after
struct ApprovalRequest final
{
    std::string  operationId{};
    ControlLease lease;
    ContentHash  policyHash;
    std::string  approverPrincipal{};
    ContentHash  approverCapabilityHash;
    uint64       expiresAtUnixMillis{};
};
```

`policyHash` stays a caller field. It is the one remaining hole and it belongs to
`c12`, not to W2 — see §10. Say so in the commit message rather than leaving it
to be discovered.

### `OperatorCoordinator::transitionOperation`

Four of the twenty-one `OperationEvent` values become effects of `freezePlan`
and `mintNextStep` and leave the caller's vocabulary: `ApprovalRequired`,
`ReadyWithoutApproval`, `NextStepApprovalRequired`, `NextStepReady`. Express
that as a type rather than as a runtime rejection list:

```cpp
// The transitions a controller may ask for by name. The plan-lifecycle events
// are absent because the Operator decides them: a caller that could say
// ReadyWithoutApproval could skip an approval the derived risk required.
enum class OperationSignal : uint8
{
    ReadCompleted,
    DecisionInputsChanged,
    Revalidated,
    Invalidated,
    Denied,
    Cancelled,
    DeadlineExpired,
    NewEvidence,
    PostDispatchAbort,
};

auto transitionOperation(
    std::string const& operationId,
    uint64 expectedRevision,
    OperationSignal signal
) -> Result<StoredOperation>;
```

Map `OperationSignal` to `OperationEvent` with a `constexpr std::array` of
pairs, as `parseOperationState` already does (`operation.cpp:145-160`) — not an
`if`/`else if` chain. `OperationEvent` itself is unchanged; `OperationMachine`
still needs all twenty-one.

### `ProjectVocabulary`

`conformance/include/conformance/provider.hpp:27-60` must
gain the plan documents. The suite may not invent project bytes, and after W2
`plugin.plan` and `plugin.next_step` produce documents the deployment's own
validator must accept:

```cpp
        // A PlanProposal this project's plugin returns for `mutatingTool`, one
        // that names `otherMutatingTool` instead (so the mint's tool check can
        // be falsified), and a UIActionIntent and a WaitIntent whose step_key
        // is in the proposal's allowed_ui_actions.
        std::string planProposal{};
        std::string mismatchedPlanProposal{};
        std::string uiActionIntent{};
        std::string waitIntent{};

        // A proposal whose workflow_limits.maximum_steps exceeds the Operator
        // ceiling, and one whose maximum_steps is 2, so that both the clamp and
        // the bound have a fixture.
        std::string oversizedPlanProposal{};
        std::string twoStepPlanProposal{};
```

## 8. Call sites that must change

`reserveDispatch` has **no non-test caller**. Every site below is a test or a
test harness. Counts observed 2026-08-10 after the last edit by another agent;
re-run these three commands before starting, because the files are moving:

```bash
rg -n "reserveDispatch|createSnapshot|issueApproval|transitionOperation" \
  --glob '!modules/operator/source/**' tests/ conformance/
rg -n "ApprovalRequest\{" tests/ conformance/
rg -n "hashOf\(\"(plan|step|decision|snapshot)" tests/ conformance/
```

| File | `reserveDispatch` | `createSnapshot` | `issueApproval` | `transitionOperation` |
|---|---|---|---|---|
| `conformance/source/suite-support.cpp` | 1 | 1 | 0 | 1 |
| `conformance/source/suite-control-ledger.cpp` | 6 | 1 | 1 | 3 |
| `tests/operator/project-fixture.hpp` | 1 | 1 | 0 | 1 |
| `tests/operator/test-ledger.cpp` | 5 | 2 | 1 | 5 |
| `tests/operator/test-control-contract.cpp` | 0 | 2 | 0 | 0 |

Also required:

- `conformance/exemplars/umbraflow/provider.cpp` and
  `conformance/exemplars/arcana-expedition/provider.cpp` — both must supply
  the new `ProjectVocabulary` members, and both fixture plugins currently return
  `'{}'` from `plan` and `next_step`
  (`tests/operator/project-fixture.hpp:654-655` shows the same in the unit
  fixture). Their `ProjectDocumentValidator` accepts only `"{}"` for
  `Plan`/`NextStep` outputs (`project-fixture.hpp:251-253`,
  `arcana-expedition/provider.cpp:375-377,393-395`); both must accept the real
  documents instead. Not "as well as" — the old spelling goes.
- `tests/operator/project-fixture.hpp` and `conformance/source/suite-support.cpp` —
  `prepareStore` must build an `OperatorPlanAuthority` and the helpers
  (`createReadyOperation`, `readyOperation`, `reconcilingOperation`) must drive
  `freezePlan` and `mintNextStep` instead of `transitionOperation` +
  three `hashOf(...)` arguments.
- `tests/CMakeLists.txt` — no change. `contract-control-c05`,
  `contract-control-c08` and `contract-state-s04` are already in
  `UF_REQUIRED_DOCTEST_CONTRACTS` (lines 61, 68, 71) and already registered
  (lines 320, 327, 330). The registry rejects any ID not in the migration
  report, so the new non-contract cases in §9 must be plain `TEST_CASE` names in
  `tests/operator/test-ledger.cpp`, not `contract-*` names.

## 9. Falsifiable tests

Every entry names one mutation. The mutation is a single-line edit to the
implementation, and the test must go red when it is applied — if it would stay
green, the test guards nothing and does not count.

| # | Case and file | Property | Single-line mutation that must turn it red |
|---|---|---|---|
| T1 | `contract-control-c05`, `test-control-contract.cpp` | The frozen plan's tool identity is the Operation's, not the plugin's. Freeze with `vocabulary.mismatchedPlanProposal` against an Operation created for `mutatingTool`; `freezePlan` must fail. | In `OperatorPlanAuthority::mintPlan`, change the guard `if (claims.toolName != inputs.invocation.toolName())` to `if (false)`. |
| T2 | `plan authority is bound to its registration`, `test-ledger.cpp` | An authority built from the foreign registration cannot freeze this Operation. | In `mintPlan`, change `if (m_projectRegistrationHash != inputs.invocation.projectRegistrationHash())` to `if (false)`. |
| T3 | `the plugin cannot widen the workflow bound`, `test-ledger.cpp` | Freeze with `oversizedPlanProposal`; assert `frozen.limits.maximumSteps == k_workflowCeiling.maximumSteps`. | In `mintPlan`, change `.maximumSteps = std::min(claims.limits.maximumSteps, k_workflowCeiling.maximumSteps),` to `.maximumSteps = claims.limits.maximumSteps,`. |
| T4 | `contract-state-s04`, `test-state-contract.cpp` | The decision basis covers only semantic input. Create two snapshots from identical `ObservedSnapshotParts` across a lease takeover (new `lease_id`, new `fencing_token`, new lease revision, new token); assert the two `decisionBasisHash` values are equal and the two `identityHash` values are not. | In `deriveDecisionBasis`, add one line `material += std::to_string(parts.fencingToken);` before the hash. |
| T5 | `the decision basis ignores revisions`, `test-ledger.cpp` | Two snapshots whose `projectObservationRevision` and `availabilityRevision` differ but whose four component hashes are equal produce the same basis. | In `deriveDecisionBasis`, add one line `material += std::to_string(parts.projectObservationRevision);`. |
| T6 | `the decision basis moves with project state`, `test-ledger.cpp` | Commit a reconciliation that changes `project_state`, take a new snapshot with identical observed parts, assert the basis changed. This is the positive control for T4 and T5: without it, a `deriveDecisionBasis` that returns a constant passes both. | In `deriveDecisionBasis`, change `appendHash(material, parts.projectStateHash);` to `appendHash(material, parts.sessionManifestHash);`. |
| T7 | `contract-control-c08`, `test-control-contract.cpp` | The workflow stops at the frozen bound and the Operation stays reconciling. Freeze `twoStepPlanProposal` (`maximum_steps` 2), run two full step/dispatch/outcome/continue cycles, then assert the third `mintNextStep` fails, the Operation is still `Reconciling`, and a second mutating Operation on the same target is still refused. | In `OperatorCoordinator::mintNextStep`, change `if (stepIndex > plan.maximumSteps)` to `if (false)`. |
| T8 | `a step cannot be replayed at another index`, `test-ledger.cpp` | Identity includes the index: the plugin returns the identical `uiActionIntent` twice; assert the two `stepIntentHash` values differ. | In `deriveStepIntent`, delete the line `material += std::to_string(stepIndex);`. |
| T9 | `only one step may await dispatch`, `test-ledger.cpp` | A second `mintNextStep` while a UI-action step is undispatched is refused. | In `mintNextStep`, change `if (pendingStepIndex.has_value())` to `if (false)`. (This is why §5 rules out a second, database-level enforcement.) |
| T10 | `a plan freezes once`, `test-ledger.cpp` | A second `freezePlan` fails and the stored plan is unchanged: assert the second call fails and `reserveDispatch` still reports the first `frozenPlanHash`. | In `freezePlan`, change `INSERT INTO operation_plans(` to `INSERT OR REPLACE INTO operation_plans(`. |
| T11 | `read-only Operations get no plan`, `test-ledger.cpp` | `freezePlan` on an Operation created from `vocabulary.readOnlyTool` fails. | In `freezePlan`, change `if (!mutating)` to `if (false)`. |
| T12 | `the effect envelope is order-independent`, `test-ledger.cpp` | Two Operations whose proposals list the same effects in different orders produce the same `effectEnvelopeHash`. | In `deriveEffectEnvelope`, delete the `std::ranges::sort(ordered, ...);` line. |
| T13 | `the dispatch records the frozen basis`, `test-ledger.cpp` | `DispatchReservation::decisionBasisHash` equals the frozen plan's, and `stepIntentHash` equals the minted step's. | In `reserveDispatch`, change the `authority_decisions` binding `plan.decisionBasisHash` to `plan.planHash`. |
| T14 | `contract-control-c12` (existing), `test-control-contract.cpp` | An approval bound to step N cannot be consumed by step N+1. The case exists; it must be rewritten to take its hashes from real mints instead of `hashOf("step")`. | In `reserveDispatch`, delete `"AND step_intent_hash=?11 "` from the `UPDATE approvals` statement. |
| T15 | `the caller cannot choose approval`, `test-ledger.cpp` | `OperationSignal` has no `ReadyWithoutApproval`: freeze a high-risk plan and assert the Operation is `AwaitingApproval` and that `reserveDispatch` without an approval fails. | In `freezePlan`, change the derived `approvalRequired` to `false` on its assignment line. |

T4 through T6 must be written as a group. An empty or constant decision basis
satisfies T4 and T5 alone; T6 is the positive control that proves the derivation
can produce a different value at all.

Existing behavioural cases that must keep passing unchanged in meaning:
`contract-control-c09`, `c10`, `c11`, `c13`, `c14`, and the ledger's
"dispatch freezes once and every Host outcome enters reconciliation". Their
call sites change shape; their assertions must not weaken.

## 10. Deliberately not in W2

- **Policy artifact evaluation.** `schema/umbraflow-policy-v1.schema.json`
  defines `PolicyArtifact` with `owned_by: "operator"` and
  `unknown_effect_decision: "deny"`, and nothing parses it. Every occurrence of
  "policy" under `modules/operator/source` is storage: `SessionManifest`'s
  `policy_artifact_hash` (`manifest.hpp:135`, `manifest.cpp:103`),
  `ApprovalRequest::policyHash` (`ledger.hpp:144`), and the `policy_hash` column
  of `approvals` (`ledger.cpp:715`, bound at `:3482`). Nothing reads a policy
  document and nothing decides on one.
  `required_approvals` is therefore derived in W2 from `risk` alone, through an
  Operator-owned table, and `ApprovalRequest::policyHash` stays a caller field.
  This is `c12`'s remaining debt, and `c12` is currently counted as done.
- **`Host::deliver` joined to the ledger** — W4.
- **Composing the snapshot parts** (`ProjectObservation`, `plugin.derive`,
  atomic publication) — W3. W2 derives the two hashes *from* the parts.
- **`maximum_observations`, `maximum_waits`, `maximum_elapsed_ms`** — stored,
  not enforced. §5 says why.
- **Binding-level validation of `allowed_ui_actions`** — the Host's, W4.

## 11. Open questions

1. **The W2/W3 boundary on the snapshot.** §4 concludes that `s04` cannot be
   closed without `createSnapshot` taking parts and deriving both hashes, which
   overlaps what `2026-08-10-next-block.md` §2 assigns to W3 ("derive the
   snapshot identity instead of accepting one"). The split proposed here — W2
   derives, W3 composes — is inferred from the schema, not stated in either
   document. Confirm before starting; it changes the size of both items.
2. **`k_workflowCeiling`'s values.** 64/64/256/64/600000 are invented here.
   Nothing in the frozen bundle, the schema or the migration report names a
   ceiling. Confirm the numbers or the source they should come from.
3. **`required_approvals` as a boolean.** The schema types it as an array of
   identifiers. W2 collapses it to 0/1 because the ledger's only approval kind
   is the single `approvals` row. If the array is meant to name distinct
   approver capabilities, the column and the `approvals` table both need a
   different shape, and that is larger than W2.
4. **Whether `next_step` is called at freeze time or after the first
   reconciliation.** The `Operation` definition requires a `current_step`
   (required at `schema/umbraflow-operator-v1.schema.json:1002`, typed at
   `:1052` as `null | UIActionIntent | WaitIntent`) but permits `null`. §5
   assumes step 1 is minted after the freeze and before the first dispatch. If
   the intent is that freezing already produces step 1, `freezePlan` and
   `mintNextStep` collapse into one call for the first step and the state
   sequence changes.
5. **Whether a wait step may be minted before any dispatch.** A wait consumes a
   step and produces no dispatch; a plan that opens with a wait would sit in
   `Ready` with a pending non-dispatchable step and no edge out. §5 assumes the
   first step is always a UI action. If waits may lead, the machine needs an
   edge this document does not specify.
6. **Where the plan authority is constructed in production.** The other four
   schema owners are built by "trusted deployment code", and no production
   deployment exists yet — every construction site today is a test fixture. This
   is not W2's to answer, but it is why the exact operator-protocol schema bytes
   have no production reader.
