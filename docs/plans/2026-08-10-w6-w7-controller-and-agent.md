# W6 Controller facade and W7 Agent subscription and budgets

Status: **landed 2026-08-11** — W6 `93698b4`, W7 `c23efd3`. Everything below is
the pre-landing specification and is left as written.
Date: 2026-08-10 (landed 2026-08-11)
Scope: `umbraflow-cpp` only. No consumer-project writes.
Closes: `p01 p02 p03` (W6), `a01 a02` (W7)
Depends on: W2 ([`2026-08-10-w2-effective-plan.md`](2026-08-10-w2-effective-plan.md))

W2 landed while this was being written. §9 records the reconciliation against
it, and the sections below already use W2's shapes. W3 and W4 are still in
flight; the assumptions about them are unverified.

Requirement wording is
[`2026-08-10-next-block.md`](2026-08-10-next-block.md) §2. Ownership and CTest
IDs are [`2026-08-09-runtime-migration-report.md`](2026-08-09-runtime-migration-report.md)
rows `P-01`, `P-02`, `P-03`, `A-01`, `A-02`. The frozen authority is
[`2026-08-09-runtime-hardening-rewrite.md`](2026-08-09-runtime-hardening-rewrite.md).

> **Amended 2026-08-10: read §1.2 of
> [the W2-W7 reconciliation](2026-08-10-w2-w7-reconciliation.md) first.** Those
> five migration report rows now read `schema-product-p01/p02/p03` and
> `schema-agent-a01/a02`; `dcc43b5` renamed them, and no `contract-` spelling of
> any of the five exists. They are not "the five IDs `tests/CMakeLists.txt`
> already requires" — W6 and W7 must each create a new `contract-*` case, with
> the migration report updated first.

> **Amended 2026-08-11: "W2 landed" above meant W2's specification; W2's code
> landed on 2026-08-11.** W3 is `4b955de` and W2 is `848e390`, so the
> assumptions this document called unverified about W3 can now be read out of
> the tree, and only W4 is still in flight. Two of W2's shapes differ from what
> §9 reconciled against — `reserveDispatch` takes no caller hashes at all, and
> `createSnapshot` is `(lease, plugin, observation)` because W3 landed first and
> `ObservedSnapshotParts` was never written. §6.3's fingerprint and table list
> are corrected there. The five rows above are still schema-only and W6 and W7
> still each owe a new `contract-*` case, migration report first — an order both
> landings inverted and had to have repaired afterwards.

> **Landed 2026-08-11 in `93698b4` (W6) and `c23efd3` (W7). Read what follows as
> the plan, not as the tree.** `p01`, `p02`, `p03`, `a01` and `a02` are closed
> and each owns a `contract-` gate. Every "Corrected 2026-08-11 against the
> landed tree" note below was written against `848e390` and predates these two
> landings; where one states a current value, read it as of that commit. The
> tree carries
> `sha256:bda31e4b18a8096b28e5208f5988dea8658bea9d7917d78cd8655d4f581a8559` over
> 23 tables, reached in two steps rather than the one §6.3 plans: W6 added
> `external_input_findings` and `ledger_events` (22 tables,
> `c691f1d9bf…`) and W7 added `agent_budgets`.
>
> **§6.1's three schema changes were all declined, which is what the
> reconciliation §7.1 recommended and is worth stating as an outcome rather than
> an omission.** `schema/` is byte-identical to `848e390`.
> `ProgressMarker.elapsed_without_progress_ms` stays and stays unconsumed;
> `maximum_no_progress_steps` was not added and the ceiling is
> `k_agentNoProgressCeiling`, Operator-owned beside the risk-unit table;
> `OperatorSession.controller_kind` landed as the `sessions.controller_kind`
> column only, so no bundle root moved and no `session_manifest_hash` moved with
> it.
>
> **What this document specified and the implementation refused.** Each refusal
> is a second spelling or a new authority channel avoided, not a corner cut.
>
> - **`ExternalInputSource` does not exist.** `p02` works by making a command
>   inexpressible rather than by tagging one: `recordExternalInput` takes an
>   action and a reason, there is no tool name, no version, no canonical
>   arguments and no overload taking a `ValidatedToolInvocation`, and
>   `external_input_findings` has no column that could hold one — asserted
>   against the stored DDL text.
> - **`AgentBudgetRemaining` carries one no-progress counter, not two.** §5.1's
>   `noProgressSteps` beside `sameStateRepetitions` are two names for one fact;
>   the tree has `consecutiveNoProgressSteps`.
> - **`budgetsRequired` stays on `ControllerProfile`, but not where §5.1 puts
>   its enforcement.** Budgets are established at `pinSession` — the door the
>   host comes through — never at `bindController`, so no entry point taking a
>   `ControllerBinding` can name, raise or refresh one.
> - **`createSnapshot` gains neither a `ControllerBinding` nor an `observedAt`.**
>   The lease already names the session, and a caller-supplied instant is a
>   caller-supplied deadline; the time budget is the coordinator's own steady
>   clock.
> - **T-A02-j is unfalsifiable as written.** Loosening
>   `CHECK(remaining_actions >= 0)` to `>= -1` cannot go red while a C++ guard
>   refuses in front of the column. The guard was removed instead: the charge is
>   an unconditional decrement and the constraint *is* the refusal, which is what
>   makes the mutation mean something.
> - `operation_state_changed` is deliberately not a fourth `ledger_events` kind.
>   One event kind with one producer would leave four other transitions silent,
>   which is a worse incompleteness than three kinds that are complete with
>   respect to themselves.
> - `agent_budgets.agent_profile_hash` was specified, written and removed: the
>   session names the manifest and the manifest names the profile.
>
> **The mutation campaigns are the payload, and W6's had to be rerun in full.**
> The harness restored files with their original modification time, so Ninja
> never rebuilt and every later run tested a mutation that had supposedly been
> reverted; it surfaced only when a post-campaign run failed against source that
> said the opposite. Both defects in the harness, and the masked-refusal defect
> W7 found in its own new case, are in
> [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md). The zeros
> these two items carry — `requireLiveBinding`'s four mutually masking
> conjuncts, and the budget-presence invariant that cannot be turned red because
> both of its sides move together — are recorded beside their requirements in
> [the next block](2026-08-10-next-block.md) §2.

W7 is specified with W6 because `ExternalInputFinding.detected_after_cursor` is
a `SubscriptionCursor` (`schema/umbraflow-operator-v1.schema.json:269`). W6
cannot record a finding without the cursor. **Ruling: W6 introduces the
append-only event sequence that *is* the cursor; W7 adds `subscribe` and the
budgets on top.** The W7-depends-on-W6 order in §3 of the next-block plan
stands.

## 0. Ground truth this design was checked against

| Claim | Verified at |
|---|---|
| No controller facade, no `ControllerKind`, no `ControllerCapability` in C++ | `grep ControllerCapability modules/` returns nothing |
| `ProjectSnapshot`, `SubscriptionCursor`, `AgentBudget` are schema-only | same; only the four schema-shape tests name them |
| The operator module has **no production caller** | `grep -rl OperatorCoordinator entry/ modules/ tools/` → only `modules/operator/**` and a forward-declared `friend` at `modules/task/source/task/page-model-file.hpp:20,138` |
| Every call site is under `tests/operator/` | 27 call sites across four files plus `project-fixture.hpp` |
| `command_fingerprint` excludes `client_request_id` | `modules/operator/source/operator/ledger.cpp:2617-2625` |
| The database schema fingerprint is an unnamed inline literal | `modules/operator/source/operator/ledger.cpp:371` |
| A caller already supplies a deadline value (`expiresAtUnixMillis`) | `modules/operator/source/operator/ledger.hpp:147` |
| `agent_profile_hash` is hashed into `SessionManifest` and read by nothing | `modules/operator/source/operator/manifest.hpp:137`; no other hit |

## 1. `p01` — the one Operation path

### 1.1 What is genuinely shared

Everything between "a controller names a tool" and "the Operation reaches a
terminal disposition". Concretely, all three kinds share one `operations` row
with one `command_fingerprint`, one snapshot binding, one mutation-chain slot,
one `OperationMachine`, one dispatch reservation, one `AuthorityDecision`, one
reconciliation. There is exactly one function that creates an Operation, and it
cannot be called without a `ControllerBinding`.

### 1.2 What a controller kind may vary

Three properties, and only these three:

| Property | Script | Agent | Human | Why it is a kind property and not a capability |
|---|---|---|---|---|
| `semanticToolsOnly` | no | **yes** | no | `ControllerCapability.allowed_tools` narrows; `p03` is a ceiling no capability may raise |
| `budgetsRequired` | no | **yes** | no | Script and Human have no `agent_profile_hash` to bind budgets to |
| `mayReportExternalInput` | no | no | **yes** | a Script asserting "a human typed" would be fabricating evidence about a third party |

What a kind may **not** vary: the fingerprint material, the mutation chain, the
fence, the epoch check, the snapshot freshness rule, the state machine, the
Journal. Approval eligibility and takeover eligibility are **not** kind
properties — they are `ControllerCapability.allowed_operations` entries
(`approve`, `takeover`, already in the schema at line 118). Expressing them
twice would be the forbidden second spelling.

### 1.3 The shared path

```cpp
// modules/operator/source/operator/controller.hpp (new)
#pragma once

#include "manifest.hpp"

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <array>
#include <string>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    // The three operators the system has. It is closed: the offline
    // exploration Agent is not a fourth value, because it holds no production
    // session, lease or ledger row at all (see 3.1).
    enum class ControllerKind : uint8
    {
        Script,
        Agent,
        Human,
    };

    // The only per-kind variation the Operator recognises. Dispatch is this
    // table, never a chain of tests on the kind.
    struct ControllerProfile final
    {
        ControllerKind kind{ControllerKind::Agent};

        bool semanticToolsOnly{true};
        bool budgetsRequired{true};
        bool mayReportExternalInput{false};
    };

    inline constexpr auto k_controllerProfiles = std::array{
        ControllerProfile{ControllerKind::Script, false, false, false},
        ControllerProfile{ControllerKind::Agent,  true,  true,  false},
        ControllerProfile{ControllerKind::Human,  false, false, true },
    };

    // UF_CHECKs that the table is indexed by its own kind, so a reordered or
    // extended enum is a build-time failure rather than a silently wrong row.
    [[nodiscard]]
    auto controllerProfile(ControllerKind kind) noexcept -> ControllerProfile;

    // An authenticated controller bound to one pinned session for one process
    // epoch. It is a value, not a handle: it stores no reference to the
    // coordinator, so it cannot outlive one and cannot be a stored borrow.
    // Only OperatorCoordinator::bindController can mint one.
    class ControllerBinding final
    {
        friend class OperatorCoordinator;

        std::string     m_sessionId;
        std::string     m_controllerId;
        std::string     m_controlledTargetKey;
        ContentHash     m_capabilityProfileHash;
        ContentHash     m_agentProfileHash;
        uint64          m_sessionEpoch;
        ControllerKind  m_kind;

        ControllerBinding(
            std::string sessionId,
            std::string controllerId,
            std::string controlledTargetKey,
            ContentHash capabilityProfileHash,
            ContentHash agentProfileHash,
            uint64 sessionEpoch,
            ControllerKind kind
        );

    public:
        [[nodiscard]]
        auto sessionId() const noexcept UF_LIFETIME_BOUND -> std::string const&;

        [[nodiscard]]
        auto controllerId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]]
        auto controlledTargetKey() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto capabilityProfileHash() const -> ContentHash;
        [[nodiscard]] auto agentProfileHash() const -> ContentHash;
        [[nodiscard]] auto sessionEpoch() const noexcept -> uint64;
        [[nodiscard]] auto kind() const noexcept -> ControllerKind;
        [[nodiscard]] auto profile() const noexcept -> ControllerProfile;
    };
}
```

and, on the coordinator:

```cpp
// modules/operator/source/operator/ledger.hpp
        // The one Operation path. There is no other way to create an
        // Operation: the binding carries the authenticated identity and the
        // kind, the invocation carries the tool the catalog owner recognised,
        // and the request carries the four things a caller may say.
        [[nodiscard]]
        auto submitCommand(
            ControllerBinding const& controller,
            CommandRequest const& request,
            ValidatedToolInvocation const& invocation
        ) -> Result<AcceptedCommand>;
```

with

```cpp
    // Everything about a command that is the caller's to say. session_id is
    // gone: it now has exactly one spelling, on the binding.
    struct CommandRequest final
    {
        std::string      snapshotToken{};
        std::string      idempotencyNamespace{};
        std::string      clientRequestId{};

        // Supplied rather than read from a stored clock so the time budget is
        // testable without sleeping and the coordinator holds no clock seam.
        // ApprovalRequest::expiresAtUnixMillis is the existing precedent.
        MonotonicInstant submittedAt{MonotonicInstant::now()};
    };

    struct AcceptedCommand final
    {
        StoredOperation operation;

        // What this submission cost. Zero-filled for a non-Agent binding,
        // because a kind without budgets has nothing to report.
        AgentBudgetRemaining remaining{};
    };
```

`CommandRequest` still cannot name mutability, effect, risk, binding, Receipt,
model, frame or cycle authority. `ControllerBinding` adds no field a caller can
choose either: every member is copied out of the pinned `sessions` row.

## 2. `p02` — out-of-band human input

### 2.1 What "disguised as a ToolInvocation" means concretely

The tempting design is: a human presses a key on the controlled window; the
host turns that keystroke into a synthetic `ToolInvocation` (`ui.key`, args
`{"code":"E"}`) and pushes it through `submitCommand`, so the ledger shows a
tidy Operation. This is the design that must not exist. Four reasons, in
descending order of severity:

1. **It fabricates authority.** An Operation carries a snapshot token, a
   decision basis, an EffectivePlan and an `AuthorityDecision`. A keystroke has
   none of them: it happened, nobody planned it, nothing authorised it.
   Recording it as a command makes the ledger assert a decision that was never
   taken — the exact failure `a04` ("the Journal records only confirmed or
   provable facts") exists to prevent.
2. **It inverts causality.** A `ToolInvocation` is a *request* the Operator may
   deny. External input has already happened; there is nothing left to deny. A
   record shaped like a request implies `denied` is a reachable disposition for
   it, and it is not.
3. **It needs fields the caller is forbidden to submit.** To synthesise the
   invocation you must invent a `ToolDescriptor` — mutability included. That is
   precisely the hole `ValidatedToolInvocation` was built to close
   (`tool-invocation.hpp:22-27`).
4. **It corrupts idempotency and the mutation chain.** External input has no
   `client_request_id`; minting one makes a replay indistinguishable from a
   second keystroke. Taking the mutation chain for it would let a keystroke
   block the Operator's own dispatch, or the reverse.

### 2.2 What an external input source is instead

Two shapes, both already in the schema, and neither is an Operation:

- **Authorised human control** is a lease movement, not input:
  `takeoverLease` writes a `control_transitions` row with `transition =
  'takeover'` (`ledger.cpp:2377-2393`). It is already implemented; W6 only
  routes it through a binding.
- **Unauthorised or out-of-band input** is a *finding*: evidence that the world
  moved under us. It is recorded as `ExternalInputFinding`
  (`schema/umbraflow-operator-v1.schema.json:250-280`) and its only effect is to
  invalidate snapshots and freeze the in-flight Operation.

```cpp
    enum class ExternalInputSource : uint8
    {
        // A controller with mayReportExternalInput told us. Only a Human
        // binding may mint this.
        Reported,
        // The Operator itself concluded it: target generation changed, or an
        // observation diverged from the frozen plan's preconditions. No
        // controller mints this one.
        Observed,
    };

    enum class ExternalInputAction : uint8
    {
        FreezeAndReobserve,
        FreezeAndReconcile,
    };

    // Deliberately unable to name a tool, arguments, a snapshot token or a
    // request id. The parameter list is the guarantee: this type cannot
    // express a command, so no caller can smuggle one through this door.
    struct ExternalInputReport final
    {
        ExternalInputSource source{ExternalInputSource::Observed};
        ExternalInputAction requiredAction{ExternalInputAction::FreezeAndReobserve};
        std::string         reason{};
    };

    struct RecordedExternalInput final
    {
        std::string                findingId{};
        uint64                     detectedAfterCursor{};
        uint64                     invalidatedSnapshotRevision{};
        std::optional<std::string> operationId{};
    };

        [[nodiscard]]
        auto recordExternalInput(
            ControllerBinding const& reporter,
            ExternalInputReport const& report
        ) -> Result<RecordedExternalInput>;
```

`recordExternalInput` refuses `Reported` unless
`reporter.profile().mayReportExternalInput`, and refuses `Observed` from any
controller at all — `Observed` findings are minted internally by the same
transaction that detected the divergence.

### 2.3 How the Journal and the Ledger record it

**The Project Journal records nothing.** A `JournalEvent` requires a
`namespaced_event_type`, a `payload_schema_hash` and an
`opaque_project_payload` validated by the project's own schema
(`schema/umbraflow-journal-v1.schema.json`). A keystroke is not a project fact
and has no project payload; minting one would mean inventing a project claim
the project never made, which `a04` forbids.

**The Ledger records it in a table that cannot hold a command.** An auditor
distinguishes the two by which table the row is in, and the two column sets are
disjoint by construction:

| | `operations` | `external_input_findings` |
|---|---|---|
| has | `tool_name`, `tool_version`, `canonical_args`, `command_fingerprint`, `client_request_id`, `snapshot_token`, `mutating`, `state` | `source`, `required_action`, `detected_after_cursor`, `invalidated_snapshot_revision`, `reason` |
| written by | `submitCommand`, only with a `ValidatedToolInvocation` | `recordExternalInput`, which has no invocation parameter |

A finding has no column to put a tool name in; an Operation has no column to
put `required_action` in. Neither can be spelled in the other's shape, and the
absence is checkable by reading `sqlite_schema` (§8, T-P02-a).

Both also append one `ledger_events` row, so a subscriber sees the keystroke in
the same ordered stream as the script's command — which is the point: the Agent
must be able to notice that a human intervened.

### 2.4 Bearing of the deferred "third front-end" decision

Yes, it bears, and it is the same discipline one layer down. The standing
decision (`docs/plans/2026-08-01-three-layers-and-agent-operator.md` §一.2:
"Agent 是第三种操作者…独立的前端,自己的动词表与事件词汇"; and
`2026-08-01-agent-front-end-and-exploration.md` §一, "裸点击与 crop 有自己的事件
形状…**不得**写成 `engine.action_delivered`") says a new operator gets its own
verbs and events rather than borrowing another's vocabulary. `p02` is that rule
applied to the Operator ledger.

**Drift to record:** that decision names `trace::FrontEnd::Annotation`, and
`FrontEnd` no longer exists — `grep -rn FrontEnd modules/` returns nothing.
`modules/trace/source/trace/event.hpp` now carries `AuditMetadata::actor` and
`TraceStreamSpec::producer` instead. So the decision lands here as: the human
surface is a distinct `ControllerKind::Human` in the ledger and a distinct
`producer` in the Trace stream, never a `ControllerKind::Script` submitting
synthetic clicks. Someone should reconcile the older documents' `FrontEnd`
wording; this specification does not edit them.

## 3. `p03` — semantic tools only

### 3.1 First, which Agent

`docs/plans/2026-08-01-three-layers-and-agent-operator.md` §二 gives the Agent
*more* privilege than a script — arbitrary coordinates, arbitrary pixels. That
is the **offline exploration** Agent, which lives in the authoring capability
root, writes `annotation-workspace.sqlite`, and by `a06` is isolated from
production. It holds no `OperatorSession`, no lease and no ledger row, so it is
not on this path and is not a fourth `ControllerKind`.

`p03` is about the **online** Agent: one attached to a production session,
driving a live target through the Operator. That one gets semantic tools only.
The two are not in conflict; the word "online" carries the whole distinction
and this specification is the first place it is written down.

### 3.2 The exact rule, in existing vocabulary

`ToolMutability` cannot express it. A read-only tool can be entirely
non-semantic ("hand me the raw frame"), and a mutating tool can be perfectly
semantic. Overloading mutability would give one field two meanings. **A new
descriptor field is required.**

```cpp
// modules/operator/source/operator/tool-invocation.hpp
    // Whether a tool's arguments and results are stated in the project's own
    // vocabulary, or in the machine's -- coordinates, pixels, key codes,
    // receipts, fencing tokens, bindings, frames. It is a property of the Tool
    // Catalog descriptor and never of a request.
    enum class ToolSurface : uint8
    {
        Semantic,
        Privileged,
    };

    struct ToolDescriptor final
    {
        std::string    toolVersion{};

        ToolMutability mutability{ToolMutability::Mutating};

        // Privileged is the default for the same reason Mutating is: a
        // descriptor that failed to state a surface gets the more restricted
        // of the two.
        ToolSurface    surface{ToolSurface::Privileged};
    };
```

`ValidatedToolInvocation` gains `m_surface` and `auto surface() const noexcept
-> ToolSurface`, exactly as it already carries `m_mutability`.

The rule, stated once:

> A tool is **offered** to a controller when
> `capability.allowed_tools` contains its name **and**
> (`controller.profile().semanticToolsOnly` implies
> `descriptor.surface == ToolSurface::Semantic`).
> A tool is **accepted** under the identical predicate, re-evaluated at
> submission.

Both halves are required. Offering less is not enforcement: a controller can
name a tool it was never offered. Offer-side lives in
`ProjectSnapshot.available_tools` (schema line 419, computed by the Operator,
never by the caller); accept-side lives in `submitCommand`.

The capability narrows and never widens: a `ControllerCapability` listing a
`Privileged` tool for an Agent grants nothing, because the predicate is a
conjunction. This is why `ToolSurface` is a ceiling on the descriptor rather
than an entry in the capability document.

### 3.3 What an online Agent must never see

- Anything naming a coordinate, pixel, frame, key code or scan code. The
  Agent's whole safety argument is that it can act only through a Binding the
  model authorises; a coordinate tool is that argument's bypass.
- Anything naming a `Receipt`, `DeliveryAuthority`, `binding_ref`,
  `fencing_token` or `authority_decision_id`. Those are Host and Operator
  authority values; a tool that *accepts* one lets the Agent forge the
  authorisation instead of earning it. The existing schema-shape gate already
  asserts `ControllerCapability` mentions no `receipt`, `coordinate` or
  `native_input` (`tests/operator/test-product-contract.cpp:195-197`).
- Anything naming a mutability, effect type, risk or artifact hash as an
  argument — the standing repository rule, for every caller, not only Agents.
- The authoring capability root and `annotation-workspace.sqlite` (`a06`).

The reason, in one sentence: the Agent is the only controller whose *intent*
the Operator cannot verify before the fact, so it is given only the surface
where every argument is a name the project's model already defines and every
effect is one the EffectivePlan can enumerate.

## 4. `a01` — snapshot plus `subscribe(after_cursor)`

### 4.1 What a cursor is and what it is derived from

A `SubscriptionCursor` is the `ledger_events.sequence` of the last event a
reader has consumed. `0` means "before the first event". It is derived from one
new append-only table with a single `INTEGER PRIMARY KEY AUTOINCREMENT`
counter, into which every controller-visible fact is appended **in the same
transaction that causes it**.

Today there is no such counter: `control_transitions.sequence` and
`reconciliations.sequence` are two independent `AUTOINCREMENT` columns
(`ledger.cpp:614`, `:743`) and `journal_events.sequence` is per project
instance. Three unrelated counters cannot be one cursor.

### 4.2 What "after" means under concurrent appends

`after` means *strictly greater than*, in commit order. The guarantee rests on
two facts about the existing store, both load-bearing and both worth naming:

1. SQLite allows one writer at a time, and every mutating coordinator method
   already opens `BEGIN IMMEDIATE` (`Transaction::begin`). Sequences are
   therefore assigned in commit order; there is no window in which a lower
   sequence commits after a higher one.
2. With `AUTOINCREMENT`, `sqlite_sequence` is updated inside the transaction and
   rolled back with it. A rolled-back append leaves no gap.

So the sequence is strictly increasing, contiguous, and never reused.

### 4.3 What the Agent is guaranteed

- Every event with `sequence > after` is returned **exactly once**, in
  increasing `sequence` order, with **no gaps** inside the returned range.
- A reader that has seen `N` will never later be shown a row with
  `sequence <= N`.
- If the request cannot be served losslessly, it gets `ResyncRequired`
  (`schema` line 1130) rather than a truncated batch. Two producible causes:
  `after < oldestAvailableCursor` (a future pruning pass), and
  `after > currentCursor` (a cursor from another database or another epoch).
  The second is producible today, so the branch is not a promise with no code.
- Snapshot and cursor are read in **one transaction**, so nothing can commit
  between the snapshot and the cursor it carries. That single sentence is the
  whole of `a01`: `snapshot.eventCursor` is the exact join point.

### 4.4 Pull, not callback

**Pull, and it must be.** Three reasons:

1. A callback stored by the coordinator is a stored borrow with no
   backing-owner lifetime contract, and it would outlive its Agent by
   construction. The repository forbids stored or asynchronous work retaining a
   reference capture or a bare `this`.
2. A callback would run Agent code inside the ledger's write transaction, where
   the fence and mutation-chain invariants live.
3. The coordinator would then hold per-subscriber state, which is a second
   place authority can leak from — the same objection §4 of the next-block plan
   already ruled against for a second trusted object.

**The subscription is the cursor.** The Operator stores nothing per subscriber;
the entire subscription state is the integer the Agent keeps.

```cpp
    struct SubscriptionCursor final
    {
        uint64 value{};

        auto operator<=>(SubscriptionCursor const&) const = default;
    };

    enum class LedgerEventKind : uint8
    {
        OperationCreated,
        OperationStateChanged,
        ControlTransitioned,
        ExternalInputDetected,
        ProjectStateAdvanced,
    };

    // Semantic by construction: it names no receipt, coordinate, fencing
    // token, plan hash or canonical argument, so handing one to an online
    // Agent cannot widen p03.
    struct LedgerEvent final
    {
        uint64          sequence{};
        LedgerEventKind kind{LedgerEventKind::OperationCreated};
        std::string     controlledTargetKey{};
        std::string     subjectId{};
        OperationState  operationState{OperationState::Proposed};
        uint64          projectStateRevision{};
    };

    struct SubscriptionBatch final
    {
        std::vector<LedgerEvent> events{};
        SubscriptionCursor       currentCursor{};
    };

    struct ResyncRequired final
    {
        SubscriptionCursor requestedCursor{};
        SubscriptionCursor oldestAvailableCursor{};
        SubscriptionCursor currentCursor{};
    };

    using SubscriptionRead = std::variant<SubscriptionBatch, ResyncRequired>;

        // Reads forward from a cursor. It is named subscribe because that is
        // the requirement's word for the cursor protocol; it registers
        // nothing, blocks on nothing, and stores nothing.
        [[nodiscard]]
        auto subscribe(
            ControllerBinding const& controller,
            SubscriptionCursor after,
            uint32 maximumEvents
        ) -> Result<SubscriptionRead>;
```

`subscribe` is scoped to the binding's `controlled_target_key`, **not** to the
binding's own session. An Agent that could only see its own events could not
notice a human takeover, which is the one thing it most needs to notice.

## 5. `a02` — the five budgets

### 5.1 Where they come from

From the `agent_profile_hash` already hashed into `SessionManifest`
(`manifest.hpp:137`) and today read by nothing. W7 consumes it, closing a
dangling proof field: `bindController` for `ControllerKind::Agent` requires the
**exact profile bytes** and verifies their SHA-256 against the manifest's hash,
using the same pattern as `ProjectToolCatalogSchemaOwner::create` requiring
`exactToolCatalogBytes` (`tool-invocation.hpp:105-114`).

```cpp
    struct AgentBudget final
    {
        uint64 maximumActions{};
        uint64 maximumMutations{};
        uint64 maximumRiskUnits{};
        uint64 maximumObservations{};
        uint64 maximumElapsedMillis{};
        uint64 maximumNoProgressSteps{};
    };

    struct AgentBudgetRemaining final
    {
        uint64 actions{};
        uint64 mutations{};
        uint64 riskUnits{};
        uint64 observations{};
        uint64 elapsedMillisRemaining{};
        uint64 noProgressSteps{};
        uint64 sameStateRepetitions{};
    };

    using AgentProfileValidator = std::function<
        Result<AgentBudget>(std::string_view exactProfileJcs)
    >;

        [[nodiscard]]
        auto bindController(
            std::string const& sessionId,
            MonotonicInstant openedAt,
            std::string_view exactAgentProfileBytes,
            AgentProfileValidator const& validateProfile
        ) -> Result<ControllerBinding>;
```

A Script or Human binding passes empty bytes and a validator that is never
called; `budgetsRequired` is false for those rows, so no `agent_budgets` row is
written. Risk maps to units by one table over W2's `Risk` enum: `read_only = 0,
low = 1, medium = 3, high = 9, critical = 27`.

### 5.2 The five axes

| Budget | Counts | Decremented by | Stored in | At exhaustion | Survives restart |
|---|---|---|---|---|---|
| action | accepted commands, read-only included | `submitCommand`, in the same transaction as the `operations` insert | `agent_budgets.remaining_actions` | `submitCommand` fails `ActionRejected`; **no** `operations` row is written | no |
| action — mutating subset | accepted commands whose `ValidatedToolInvocation::mutability()` is `Mutating` | `submitCommand`, same transaction, same row | `remaining_mutations` | as above | no |
| risk | `riskUnits(FrozenPlan::risk)` summed over frozen plans | `freezePlan`, in its transaction (see §9.1 — risk does not exist before the plan is minted) | `remaining_risk_units` | `freezePlan` fails `ActionRejected`; the Operation stays `Proposed` with no plan row | no |
| observation | snapshots composed for this binding | `createSnapshot`, in its own transaction | `remaining_observations` | `createSnapshot` fails `ActionRejected` | no |
| time | `submittedAt`/`observedAt` minus `openedAt` | not decremented — compared against `deadline_steady_millis` | `agent_budgets.deadline_steady_millis` | every budgeted entry point fails `Timeout` | no |
| no-progress | consecutive steps that changed neither fingerprint (§5.3) | `submitCommand`, before the insert, from the stored marker | `remaining_no_progress_steps`, with `same_state_repetitions` | `submitCommand` fails `ActionRejected` | no |

The mutating sub-count is a ceiling *inside* the action axis, not a sixth axis,
and it is not a second spelling of risk. W2 §4 rules that a plugin can
under-declare its own effects and that "nothing in W2 prevents this", so
`EffectivePlan.risk` is plugin-declared and cannot be trusted as a mutation
proxy. `ToolMutability` is sourced independently, from the Tool Catalog
descriptor, and it is what `operations.mutating` and the mutation chain already
run on (`ledger.cpp:2611`, `:2773-2827`). Two differently sourced counts on a
partially overlapping set are two facts, not two names for one.

Every remaining column is `CHECK(... >= 0)` on a STRICT table, so a decrement
past zero is a database error rather than a wrapped `uint64`.

**Why they are in the database and not in memory.** The decrement must be
atomic with the accept: a counter in `Impl` could drift from what was actually
inserted after any failed commit, and the ledger could no longer prove at audit
time why a command was refused. The coordinator is the single writer, so a
second in-memory authority is exactly the leak §4 of the next-block plan
already ruled out.

**Why they do not survive a restart.** They cannot: a restart begins a new
`session_epoch` (`c02`, `beginSessionEpoch` at `ledger.cpp:995`), every
coordinator entry point already refuses a session from a prior epoch
(`ledger.cpp:2157`, `:2303`, `:2653`), and so a `ControllerBinding` cannot be
reopened. The old `agent_budgets` rows become inert history. Budgets follow
control, and control already resets on restart.

The residual is stated rather than hidden: an operator who restarts the host to
give a stuck Agent a fresh budget can do so. That is deliberate out-of-band
human action, it is not available to the Agent, and it is recorded — new
`session_epoch`, new `ControlTransition`, new `ledger_events` rows.

### 5.3 Progress, defined

A step makes **progress** if and only if, at the moment `submitCommand`
accepts it, at least one of the two fingerprints differs from the one stored by
the previous accepted step:

- `state_fingerprint` — **is** `SnapshotRecord::decisionBasisHash`, the value W2
  derives inside `createSnapshot` from the four semantic hashes and stores in
  `snapshots.decision_basis_hash`. W7 defines no second composition and reads
  the column. This is *the world is different*.
- `command_fingerprint` — the existing `sha256(tool \0 version \0 args)`
  (`ledger.cpp:2617-2625`). This is *the Agent asked for something different*.

Otherwise the step is a repetition: `same_state_repetitions` increments and
`remaining_no_progress_steps` decrements. A step that makes progress resets
both.

Why *either*, not *both*: state alone would punish an Agent legitimately
probing three different tools against an unchanging screen; command alone would
let an Agent loop `observe → observe → observe` forever. Requiring either to
differ means the Agent is stuck only when it asks the same thing of the same
world.

One non-obvious property makes this work, and it is already true in the tree:
`command_fingerprint` excludes `client_request_id` (see the comment at
`ledger.cpp:2613-2616`). Resubmitting the identical command under a fresh
request id therefore yields the identical fingerprint and correctly counts as
no progress.

**Ruling:** no-progress is counted in *steps only*. `ProgressMarker` currently
also carries `elapsed_without_progress_ms` with no ceiling anywhere; a second
millisecond ceiling beside `maximum_elapsed_ms` would be two spellings of one
axis, and an unconsumed proof field fails the migration report's gate. The
field is removed (§6). The alternative is left open in §10.

## 6. DDL and schema changes, exactly

### 6.1 `schema/umbraflow-operator-v1.schema.json`

1. `OperatorSession` — add to `required` and `properties`:
   `"controller_kind": { "enum": ["script", "agent", "human"] }`.
2. `AgentBudget` — add
   `"maximum_no_progress_steps": { "type": "integer", "minimum": 1 }` to
   `required` and `properties`. `maximum_mutations` stays and is consumed
   (§5.2).
3. `ProgressMarker` — remove `elapsed_without_progress_ms` from `required` and
   `properties` (§5.3).
4. `ProjectSnapshot` — no change; `available_tools` and `event_cursor` already
   exist (lines 419, 424).
5. `ExternalInputFinding`, `ControlTransition`, `ControllerCapability`,
   `SubscriptionCursor`, `ResyncRequired` — no change; they are the shapes this
   work implements.

None of the added words collide with `FORBIDDEN_SCHEMA_WORDS` in
`tests/test-runtime-surface.py:92-116`.

There is **no compiled constant for this schema file**: `SCHEMA_AUTHORITIES`
(`tests/test-runtime-surface.py:143-164`) covers the trace, runtime-artifact,
runtime-v2 and annotation-workspace schemas only, and
`SessionManifestSpec::operatorProtocolSchemaHash` is caller-supplied. Nothing in
C++ needs recomputing for the JSON change. Real deployments' `session_manifest_hash`
values change, which is correct and intended.

### 6.2 `operator-runtime.sqlite`

Because nothing is released, these are edits to the `CREATE TABLE` text in
`OperatorCoordinator::open`, not `ALTER TABLE` migrations. They compose with
W2 §6, which adds `operation_plans`, `operation_steps` and two columns of its
own to `snapshots`.

**Changed:**

```sql
-- sessions: one new column
    controller_kind TEXT NOT NULL
        CHECK(controller_kind IN ('script', 'agent', 'human')),

-- snapshots: one new column, beside W2's decision_basis_hash and
-- canonical_parts
    event_cursor INTEGER NOT NULL CHECK(event_cursor >= 0),
```

**New:**

```sql
CREATE TABLE IF NOT EXISTS ledger_events(
    sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
    controlled_target_key TEXT NOT NULL,
    kind TEXT NOT NULL CHECK(kind IN (
        'operation_created', 'operation_state_changed', 'control_transitioned',
        'external_input_detected', 'project_state_advanced'
    )),
    subject_id TEXT NOT NULL,
    operation_state TEXT,
    project_state_revision INTEGER
) STRICT;

-- Deliberately has no tool_name, tool_version, canonical_args,
-- command_fingerprint, client_request_id or snapshot_token column. The absence
-- is the p02 guarantee and is asserted directly against sqlite_schema.
CREATE TABLE IF NOT EXISTS external_input_findings(
    finding_id TEXT PRIMARY KEY,
    controlled_target_key TEXT NOT NULL,
    session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
    reporter_session_id TEXT REFERENCES sessions(session_id),
    source TEXT NOT NULL CHECK(source IN ('reported', 'observed')),
    detected_after_cursor INTEGER NOT NULL CHECK(detected_after_cursor >= 0),
    invalidated_snapshot_revision INTEGER NOT NULL
        CHECK(invalidated_snapshot_revision >= 0),
    operation_id TEXT REFERENCES operations(operation_id),
    required_action TEXT NOT NULL
        CHECK(required_action IN ('freeze_and_reobserve', 'freeze_and_reconcile')),
    reason TEXT NOT NULL
) STRICT;

CREATE TABLE IF NOT EXISTS agent_budgets(
    session_id TEXT PRIMARY KEY REFERENCES sessions(session_id),
    agent_profile_hash TEXT NOT NULL,
    deadline_steady_millis INTEGER NOT NULL,
    remaining_actions INTEGER NOT NULL CHECK(remaining_actions >= 0),
    remaining_mutations INTEGER NOT NULL CHECK(remaining_mutations >= 0),
    remaining_risk_units INTEGER NOT NULL CHECK(remaining_risk_units >= 0),
    remaining_observations INTEGER NOT NULL CHECK(remaining_observations >= 0),
    remaining_no_progress_steps INTEGER NOT NULL
        CHECK(remaining_no_progress_steps >= 0),
    last_state_fingerprint TEXT NOT NULL,
    last_command_fingerprint TEXT NOT NULL,
    same_state_repetitions INTEGER NOT NULL CHECK(same_state_repetitions >= 0)
) STRICT;
```

`reporter_session_id` is nullable because an `observed` finding has no
reporter; `NOT NULL` would force the Operator to name a controller for a
conclusion no controller reached.

### 6.3 The fingerprint that must be recomputed

`OperatorCoordinator::open` verifies the exact schema bytes at
`modules/operator/source/operator/ledger.cpp:337-382`. Two things change:

1. **The expected fingerprint literal.** It is
   `constexpr auto k_exactSchemaV1Fingerprint` in the file's anonymous
   namespace, holding
   `"sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4"`.
   Recompute it from a freshly created database, never by hand: the canonical
   string is the *stored* DDL text, indentation and comments included.
2. **The expected table list**, which over the 20 tables W3 and W2 left plus
   W6/W7's three becomes, sorted:
   `agent_budgets,approvals,authority_decisions,control_leases,control_transitions,dispatches,external_input_findings,fencing_high_water,journal_events,ledger_events,operation_plans,operation_steps,operations,project_instances,project_observations,project_registrations,project_state,reconciliations,runtime_artifacts,runtime_installations,runtime_state,sessions,snapshots`

> **Corrected 2026-08-11 against the landed tree** (`4b955de`, `848e390`).
> Three things changed under this section. The literal was unnamed and is now
> `k_exactSchemaV1Fingerprint`, promoted by `848e390` — W6 inherits the named
> constant rather than creating it, and the name is not the
> `k_operatorDatabaseSchemaHash` this document proposed. The value moved from
> `5738e6f9…` to the one above. And the list this section originally gave was
> wrong in two ways at once: it omitted W3's `project_observations`, which
> [the reconciliation](2026-08-10-w2-w7-reconciliation.md) §6.3 had already
> caught, and it carried `runtime_publications`, which `848e390` deleted. The
> corrected list above is 23 tables; the reconciliation's §6.3 is the same
> arithmetic and remains the place where the per-landing counts live.

`k_operatorDatabaseSchemaHash` must **not** be added to `SCHEMA_AUTHORITIES` in
`tests/test-runtime-surface.py`: that gate hashes a *file*, and this is a
fingerprint over `sqlite_schema` rows. Adding it there fails the gate
permanently.

## 7. Signature changes and every call site

### 7.1 Before and after

| Before | After |
|---|---|
| `auto acquireLease(std::string const& sessionId) -> Result<ControlLease>` | `auto acquireLease(ControllerBinding const& controller) -> Result<ControlLease>` |
| `auto takeoverLease(std::string const& sessionId, std::string const& reason) -> Result<ControlLease>` | `auto takeoverLease(ControllerBinding const& controller, std::string const& reason) -> Result<ControlLease>` |
| W2's `auto createSnapshot(ControlLease const& lease, ObservedSnapshotParts const& observed) -> Result<SnapshotRecord>` | `auto createSnapshot(ControllerBinding const& controller, MonotonicInstant observedAt, ControlLease const& lease, ObservedSnapshotParts const& observed) -> Result<SnapshotRecord>` |
| `auto createOrLoadOperation(CommandRequest const&, ValidatedToolInvocation const&) -> Result<StoredOperation>` — W2 leaves it unchanged | `auto submitCommand(ControllerBinding const&, CommandRequest const&, ValidatedToolInvocation const&) -> Result<AcceptedCommand>`; W2 §5's sequence step 1 is this function |
| W2's `auto freezePlan(std::string const& operationId, uint64 expectedRevision, ControlLease const&, ProjectPluginHandle const&, OperatorPlanAuthority const&) -> Result<FrozenPlan>` | `+ ControllerBinding const& controller` as the first parameter, so the risk budget can be charged in the same transaction |
| `struct SessionPin { … SessionMode mode; }` | `struct SessionPin { … SessionMode mode; ControllerKind kind; }` |
| `struct CommandRequest { std::string sessionId; std::string snapshotToken; … }` | `struct CommandRequest { std::string snapshotToken; …; MonotonicInstant submittedAt; }` — `sessionId` removed |
| W2's `struct SnapshotRecord { … uint64 leaseRevision; ContentHash decisionBasisHash; }` | `+ SubscriptionCursor eventCursor;` |
| `struct ToolDescriptor { std::string toolVersion; ToolMutability mutability; }` | `+ ToolSurface surface{ToolSurface::Privileged};` |
| `class ValidatedToolInvocation` | `+ ToolSurface m_surface; auto surface() const noexcept -> ToolSurface;` |
| — | `+ bindController`, `+ subscribe`, `+ recordExternalInput`, `+ remainingBudget(ControllerBinding const&) -> Result<AgentBudgetRemaining>` |

`releaseLease`, `transitionOperation`, `reserveDispatch`, `mintNextStep`,
`recordDeliveryOutcome`, `issueApproval`, `commitReconciliation`,
`pinSession(pin, manifest)`, `registerProject`, `provisionProjectInstance` keep
their shapes here. W2 changes some of them for its own reasons and W4 will
change more.

`ToolCatalogValidator`'s signature is unchanged; its `ToolDescriptor` return
gains a field, so every implementation must state `surface` or inherit
`Privileged`.

### 7.2 Call sites

There is **no production caller** (§0). Every site is a test:

| File | Sites |
|---|---|
| `tests/operator/project-fixture.hpp` | `prepareStore` (`pinSession` +`ControllerKind`, `acquireLease`, `createSnapshot`) must also mint and return a `ControllerBinding` in `PreparedStore`; `command()` drops `sessionId` and gains `submittedAt`; `createReadyOperation` and `reconcilingOperation` route through `submitCommand`; `makeProject`'s Tool Catalog validator must state `surface` for each of its four fixture tools and gain at least one `Privileged` entry for the `p03` cases |
| `tests/operator/test-ledger.cpp` | 10 × `createOrLoadOperation`, 5 × `transitionOperation`, 5 × `reserveDispatch`, 4 × `recordDeliveryOutcome`, 2 × `createSnapshot`, 1 × `acquireLease`, 1 × `takeoverLease`, 1 × `pinSession` |
| `tests/operator/test-control-contract.cpp` | 8 × `createOrLoadOperation`, 7 × `reserveDispatch`, 3 × `transitionOperation`, 3 × `recordDeliveryOutcome`, 3 × `createSnapshot`, 2 × `pinSession`, 2 × `acquireLease`, 1 × `takeoverLease` |
| `tests/operator/test-state-contract.cpp` | 4 × `createOrLoadOperation`, 1 × `takeoverLease`, 1 × `pinSession` |
| `tests/operator/test-agent-audit-contract.cpp` | through the fixture only |
| `tests/operator/test-product-contract.cpp` | `contract-product-p01/p02/p03` are rewritten (§8) |
| `tests/CMakeLists.txt` | `UF_REQUIRED_DOCTEST_CONTRACTS` and the `cpp_add_contract_suite` `CASES` list gain the new case IDs; the suite gains `operator/test-controller-contract.cpp` |
| `modules/operator/manifest.txt`, `CMakeLists.txt` | add `controller.cpp` / `controller.hpp` |

## 8. Falsifiable tests

Every row names the property and the **one-line** mutation that must turn it
red. A mutation that leaves a test green means the test does not hold the
property and the test is the defect.

| ID | Property | Exact mutation that must turn it red |
|---|---|---|
| T-P01-a | Script, Agent and Human bindings submitting the identical tool and args produce the identical `command_fingerprint` | in `submitCommand`, after `fingerprintMaterial += canonicalArgs;` insert `fingerprintMaterial.push_back(static_cast<char>(controller.kind()));` |
| T-P01-b | All three kinds contend for the same mutation-chain slot: with a non-terminal mutating Operation held by a Script, a Human's mutating submit is refused | in `submitCommand`, change `if (mutating)` to `if (mutating && controller.kind() != ControllerKind::Human)` |
| T-P01-c | All three kinds walk the same state machine: an Operation created by an Agent and one created by a Human reach `Reconciling` through the identical `OperationEvent` sequence | in `OperationMachine::transition`, change `case OperationEvent::DispatchStarted:` to return `OperationState::Confirmed` |
| T-P02-a | `external_input_findings` names no command column: `sqlite_schema.sql` for that table contains none of `tool_name`, `canonical_args`, `command_fingerprint`, `client_request_id`, `snapshot_token` | in the `external_input_findings` DDL, add `tool_name TEXT NOT NULL DEFAULT '',` — asserted by `tests/test-runtime-surface.py` reading `ledger.cpp`, so it fails on the column and not merely on the fingerprint |
| T-P02-b | `recordExternalInput` writes one finding and **zero** `operations` rows, and the finding's `detected_after_cursor` equals the cursor immediately before the call | in `recordExternalInput`, change the `detected_after_cursor` bind from the live cursor to `0U` |
| T-P02-c | An external input freezes the in-flight Operation: after the call its state is `NeedsRevalidation` and a `reserveDispatch` against the pre-finding revision fails | in `recordExternalInput`, change `OperationEvent::DecisionInputsChanged` to `OperationEvent::ReadyWithoutApproval` |
| T-P02-d | A Script binding cannot mint a `Reported` finding | in `recordExternalInput`, change `reporter.profile().mayReportExternalInput` to `true` |
| T-P03-a | An Agent binding's `submitCommand` refuses a `Privileged` descriptor; a Human binding's accepts the identical invocation | in `submitCommand`, change the surface guard's `ControllerKind::Agent` to `ControllerKind::Script` |
| T-P03-b | An Agent's `available_tools` excludes the privileged name; a Human's includes it | in `k_controllerProfiles`, change the Agent row's `semanticToolsOnly` from `true` to `false` |
| T-P03-c | A descriptor that states no surface is treated as `Privileged` | in `ToolDescriptor`, change `ToolSurface surface{ToolSurface::Privileged};` to `{ToolSurface::Semantic}` |
| T-A01-a | The snapshot/subscription join is gapless and duplicate-free: take a snapshot, append N events, `subscribe(snapshot.eventCursor)` returns exactly those N with contiguous increasing sequences, first `== cursor + 1` | in `createSnapshot`, change the cursor read to `SELECT COALESCE(MAX(sequence), 0) + 1 FROM ledger_events` |
| T-A01-b | `subscribe` past the head returns `ResyncRequired` carrying the requested and current cursors | in `subscribe`, change `if (after.value > currentCursor)` to `if (after.value > currentCursor + 1U)` |
| T-A01-c | An Agent sees events caused by *other* controllers: a Script's submit and a Human's takeover both appear in the Agent's batch | in `subscribe`'s query, add `AND subject_id IN (SELECT operation_id FROM operations WHERE session_id=?)` |
| T-A01-d | The coordinator holds no per-subscriber state: two `subscribe` calls with the same cursor return byte-identical batches | in `subscribe`, change `after.value` to `batch.currentCursor.value` for the second call by caching the last served cursor in `Impl` and reading it when `after.value == 0U` |
| T-A02-a | Action budget: with `maximumActions = 1`, the second `submitCommand` fails and `operations` still holds one row | in the budget UPDATE, change `remaining_actions = remaining_actions - 1` to `remaining_actions = remaining_actions` |
| T-A02-b | Risk budget: with `maximumRiskUnits = 1`, `freezePlan` on a `medium`-risk plan fails and writes no `operation_plans` row, while a `low`-risk plan freezes | in the risk table, change `medium` from `3` to `0` |
| T-A02-k | Mutating sub-count is independent of risk: with `maximumMutations = 0` and `maximumRiskUnits` large, a `Mutating` descriptor whose plan declares `read_only` is still refused | in `submitCommand`, change the mutation decrement's guard from `invocation.mutability() == ToolMutability::Mutating` to `plan.risk() != Risk::ReadOnly` |
| T-A02-c | Observation budget: with `maximumObservations = 1`, the second `createSnapshot` fails | in the budget UPDATE, change `remaining_observations - 1` to `remaining_observations - 0` |
| T-A02-d | Time budget is inclusive at the limit and refuses past it: submit at `openedAt + maximumElapsedMillis` succeeds, at `+1` fails `Timeout` | change `elapsedMillis > budget.maximumElapsedMillis` to `elapsedMillis >= budget.maximumElapsedMillis` (red on the first assertion) or to `elapsedMillis > budget.maximumElapsedMillis + 1000U` (red on the second) |
| T-A02-e | No-progress counts *both* axes: identical command against unchanged state is refused on the second try | in the progress predicate, change `stateChanged \|\| commandChanged` to `true` |
| T-A02-f | State counts: the identical command against a **changed** state fingerprint succeeds | in the progress predicate, change `stateChanged \|\| commandChanged` to `commandChanged` |
| T-A02-g | Command counts: a **different** command against an unchanged state fingerprint succeeds | in the progress predicate, change `stateChanged \|\| commandChanged` to `stateChanged` |
| T-A02-h | A fresh `client_request_id` does not buy progress: the same tool and args under a new request id still counts as a repetition | in the fingerprint material, append `request.clientRequestId` |
| T-A02-i | Budgets do not survive a restart: after reopening the coordinator, the pre-restart binding is unusable and a re-pinned session starts with a full budget | in `bindController`, change `sessionEpoch != m_impl->sessionEpoch` to `sessionEpoch > m_impl->sessionEpoch` |
| T-A02-j | Exhaustion is a refusal, never a wrap: an attempt to decrement below zero fails the transaction | in the `agent_budgets` DDL, change `CHECK(remaining_actions >= 0)` to `CHECK(remaining_actions >= -1)` |

CTest registration: `contract-product-p01`, `contract-product-p02`,
`contract-product-p03`, `contract-agent-a01`, `contract-agent-a02` are the five
IDs the migration report already reserves and `tests/CMakeLists.txt` already
requires. Each keeps one ID and gathers the rows above; the existing
schema-shape assertions move to `schema-product-p0x` / `schema-agent-a0x` under
W10 rather than being deleted, because the shapes still need guarding.

## 9. Assumptions about W2, W3 and W4

### 9.1 W2 — landed, and reconciled

`docs/plans/2026-08-10-w2-effective-plan.md` exists. Four of this
specification's original assumptions were wrong and the sections above have
already been corrected; they are listed here so a reviewer can see what moved.

| Original assumption | What W2 actually says | What changed here |
|---|---|---|
| the plan is minted inside the command-submission call | `createOrLoadOperation` is **unchanged**; a new `freezePlan` mints the plan (W2 §5) | the risk budget is charged in `freezePlan`, not in `submitCommand` (§5.2); `freezePlan` gains a `ControllerBinding` parameter (§7.1) |
| W2 leaves `createSnapshot`'s parameter list alone | it becomes `createSnapshot(lease, ObservedSnapshotParts const&)` and `SnapshotRecord` gains `decisionBasisHash` (W2 §7) | §7.1 states the merged signature; `eventCursor` is an addition to W2's record, not to today's |
| W7 must define a `state_fingerprint` composition | W2 derives `decision_basis_hash` in `createSnapshot` and stores it in `snapshots.decision_basis_hash` | `state_fingerprint` **is** `SnapshotRecord::decisionBasisHash`; W7 defines nothing (§5.3) |
| the Operator refuses a `read_only` risk for a `Mutating` descriptor, so risk bounds mutations | "**Nothing in W2 prevents this.** The plugin is the only authority on what its own tools do" (W2 §4) | `maximum_mutations` is kept and consumed rather than removed (§5.2, §6.1), and T-A02-k falsifies the independence |

Still assumed, and consistent with W2 as read:

1. W2's `reserveDispatch` still takes a `ControlLease`, and W6/W7 do not touch
   `reserveDispatch` or `mintNextStep`.
2. `freezePlan` runs in one `BEGIN IMMEDIATE` transaction that can also carry
   the risk decrement. If it cannot, the risk budget has no atomic home and
   must move to `mintNextStep`.
3. W2's `WorkflowLimits` clamp on one Operation and the `AgentBudget` clamp on
   one binding are different scopes and do not collapse into each other: a
   single Operation cannot exhaust the binding, and a binding's budget cannot
   raise an Operation's limits.

### 9.2 W3 and W4 — unverified

**W3 (Snapshot Coordinator)**

4. `createSnapshot` still returns one value per call and can carry
   `eventCursor`; the cursor is read in the same transaction as W2's derived
   identity.
5. W3 owns `ProjectSnapshot.available_tools`. W6 supplies the *predicate*
   (§3.2) and W3 calls it rather than computing a second list. **This is the
   most likely reconciliation conflict between the two work items.**
6. `ProjectObservation` does not add a per-kind observation path; all three
   kinds see the same snapshot content.

**W4 (delivery join)**

7. `takeoverLease` keeps its `(session, reason)` shape apart from taking a
   binding.
8. W4's `recordDeliveryOutcome` appends a `ledger_events` row for the state
   change it causes. Without that the Agent's subscription silently misses
   every delivery outcome, and T-A01-c would pass while the stream was
   incomplete.

**Shared**

9. W2, W3, W4, W6 and W7 all change the DDL. Whoever lands last recomputes
   `k_operatorDatabaseSchemaHash` once; §6.3's rename to a named constant
   should land in whichever of them lands first. W2 already carries the same
   instruction, so the two must not both introduce a differently named
   constant.
10. W10 *renames* the schema-shape gates rather than deleting them.

## 10. Open questions

1. **Who is trusted to mark a tool `Semantic`?** The Tool Catalog is
   project-owned (`tool_catalog_hash` in the ProjectRegistration), so a project
   could mark everything `Semantic` and dissolve `p03`. Either the upstream
   accepts the project's own catalog as the Agent ceiling, or the Operator needs
   an independent allowlist keyed by capability. Not resolvable without the
   v1.9 clause behind `P-03`.
2. **Can a `SessionMode::Read` session bind an Agent?** `acquireLease` requires
   `mode='write'` (`ledger.cpp:2139`) and `createSnapshot` requires an active
   lease, so an observing Agent has no way to obtain a snapshot today. Either
   read sessions get a lease-free snapshot path or observation is a write-mode
   privilege. Undecided.
3. **Does the offline exploration Agent ever get a production binding?** §3.1
   says no. If it ever must, that is a new work item, not a fourth
   `ControllerKind`.
4. **Should no-progress also carry a millisecond ceiling?** §5.3 rules it out
   as a second spelling of the time axis, and removes
   `ProgressMarker.elapsed_without_progress_ms` accordingly. An Agent making
   progress for ten minutes and one stuck for thirty seconds are genuinely
   different situations, so this ruling may be wrong. If it is reversed, the
   field returns and `AgentBudget` gains
   `maximum_no_progress_elapsed_ms` beside `maximum_no_progress_steps`.
5. **Who prunes `ledger_events`?** Until something does,
   `oldest_available_cursor` is always `0` and only the
   `after > current` branch of `ResyncRequired` is producible. Retention may
   belong to W8 (artifact GC by database refcount) or to W7. Unassigned.
6. **Is `detected_after_cursor` the cursor before or after the finding's own
   append?** This specification chose *before*, so a finding never claims to
   have been detected after itself. Cheap to change; state it once and stop.
