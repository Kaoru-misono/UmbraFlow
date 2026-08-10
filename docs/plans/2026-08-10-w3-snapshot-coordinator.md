# W3: the Snapshot Coordinator

Status: specification; no code changed on its account
Date: 2026-08-10
Scope: `umbraflow-cpp` only. No consumer-project writes.
Closes: `s01` (five state kinds have separate owners), `s02` (the Snapshot
Coordinator publishes a complete snapshot atomically)
Depends on: W2 (`docs/plans/2026-08-10-w2-effective-plan.md`), specified in
parallel. Every assumption about it is listed in §8 and nowhere else.
Authority: [`2026-08-09-runtime-hardening-rewrite.md`](2026-08-09-runtime-hardening-rewrite.md)
and the frozen v1.9 bundle it pins, root
`c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`.

The work item is [`2026-08-10-next-block.md`](2026-08-10-next-block.md) §3 row
W3. Its two requirements are §2 rows `s01` (**absent** — `ProjectObservation`
does not exist) and `s02` (**partial** — `createSnapshot` takes a caller
identity, composes nothing).

## 1. The five state kinds

The enumeration is not in the upstream execution profile. It is in the frozen
bundle, at `requirements-traceability.md` line 80 and
`umbraflow-game-automation-final-design.md` §7. Both are quoted verbatim,
because paraphrase is how an owner column drifts.

Requirement S-01:

```text
| S-01 | UIObservationSnapshot、ProjectObservation、ProjectState、ControlState、
OperationState 分 owner | `REQUIRED_CORE` | 任一字段只有一个 writer；项目状态不能
反写为 UI evidence |
```

Design §7, "五种动态状态/视图必须分开":

```text
| 状态/视图 | 含义 | 唯一 Owner | Revision 变化条件 |
| UIObservationSnapshot | Host observation identity、reader/evidence refs、
  StateResolution（ordered surface stack） | Annotation Runtime | 每次新 capture/resolution |
| ProjectObservation | 项目候选、证据解释、confidence/conflicts/uncertainty 的
  opaque payload | selected ProjectPlugin.derive | 任一 UI/artifact/plugin/project-state 输入变化 |
| ProjectState | 插件定义的已确认领域状态；核心只存 canonical bytes/hash/revision |
  selected ProjectPlugin.reduce | genesis baseline、Continue/Confirmed、
  correction/divergence 提交的可证明 JournalEvent |
| ControlState | lease、policy、session、availability | Operator Runtime |
  acquire/takeover/policy/availability 变化 |
| OperationState | invocation、plan、dispatch rows、reconciliation、result |
  Operator Runtime | Operation 生命周期变化 |
```

The same section fixes the coordinator's role:

```text
进程内 Snapshot Coordinator 是唯一 snapshot head writer：它在 target mutation gate 下
读取各 owner 的不可变 revision，发布一次完整记录后再返回 token；调用者不能自行拼接来自
不同时刻的 UI/Project/Control revision。
```

English rendering, with the lifetime and the write authority each kind has in
this repository:

| Kind | Owner (design) | Lifetime | Who may write it |
|---|---|---|---|
| `UIObservationSnapshot` | Annotation Runtime | one capture/resolution cycle; superseded by the next cycle, never mutated | the trusted Luau resolver, through the Host's private capability surface |
| `ProjectObservation` | the selected `ProjectPlugin.derive` | one derive result; a new revision whenever any UI/artifact/plugin/ProjectState input changed | `derive`, invoked only by the Snapshot Coordinator |
| `ProjectState` | the selected `ProjectPlugin.reduce` | durable, per `(plugin_id, project_instance_key)`, monotonic revision from 0 | `reduce`, invoked only by `provisionProjectInstance` (revision 0) and `commitReconciliation` |
| `ControlState` | Operator Runtime | per session epoch; leases die at restart, the fencing high-water does not | `beginSessionEpoch`, `pinSession`, `acquireLease`, `takeoverLease`, `releaseLease` |
| `OperationState` | Operator Runtime | per Operation, from `proposed` to a terminal disposition | `createOrLoadOperation`, `transitionOperation`, `reserveDispatch`, `recordDeliveryOutcome`, `commitReconciliation` |

### Where the design and the code disagree

The design's list is the target. The code is behind it in two places and
short of the design in a third.

| Kind | Code today | Verdict |
|---|---|---|
| `UIObservationSnapshot` | no C++ type, and no value crosses into `modules/operator`. Resolution lives in `modules/task/runtime/resolution.luau` and `observe.luau` and never leaves Luau. | **absent** at the Operator boundary |
| `ProjectObservation` | no type. `ProjectPluginHandle::derive` exists (`modules/operator/source/operator/project-plugin.hpp:187`) and is called from `tests/operator/test-project-plugin-contract.cpp` only — `rg '\.derive\(' modules/` finds nothing. | **absent**; this is the whole of `s01` |
| `ProjectState` | the `project_state` table (`ledger.cpp:750`), written by `provisionProjectInstance` (`ledger.cpp:1910`) and `commitReconciliation` (`ledger.cpp:3830`), both through `reduce` | **present and correct** |
| `ControlState` | `sessions`, `control_leases`, `fencing_high_water`, `control_transitions`. Policy and availability have no storage. | **partial**: the design's `ControlState` includes policy and availability; neither exists |
| `OperationState` | `operations`, `authority_decisions`, `dispatches`, `approvals`, `reconciliations` | **present** |

W3 closes the first two rows. The `ControlState` shortfall is not W3's: policy
belongs to `c12`, availability to W6. §3 step 12 says what W3 writes into
`availability_revision` in the meantime and §9 flags it.

### What the schema already says

No file under `schema/` changes for W3. `schema/umbraflow-operator-v1.schema.json`
already defines `SnapshotParts` (15 required members), `SnapshotIdentity` (those
15 plus `token_id`, `session_id`, `snapshot_revision`), `ProjectSnapshot`,
`DecisionBasis` and an opaque-string `SnapshotToken`. `ProjectState` and
`ProjectInstance` are in `schema/umbraflow-journal-v1.schema.json`.

That is exactly why `contract-state-s01` and `contract-state-s02` are green
against absent behaviour: both read those files and assert a definition exists
with certain members (`tests/operator/test-state-contract.cpp:165-191`). The
shape was right before the code was. §7 replaces those two cases.

Note the split the schema already makes and which §4 depends on: `SnapshotParts`
carries only composed state, while `token_id`, `session_id` and
`snapshot_revision` are `SnapshotIdentity`-only. The identity hash therefore
covers the parts and not the record's own naming, so it can be computed before a
token is minted, and two snapshots over identical composed state share it.

## 2. `ProjectObservation`

Two values are new. `task::UiObservationSnapshot` is what the Host hands in;
`operator_runtime::ProjectObservation` is what the coordinator mints from it.
Neither is a second trusted object: `TaskHost` and `OperatorCoordinator` already
exist, and each new value's only constructor is private to the one that already
owns that state kind. The pattern is `ValidatedToolInvocation`
(`modules/operator/source/operator/tool-invocation.hpp:43-79`) and, on the task
side, `RuntimeModelBinding` (`modules/task/source/task/page-model-file.hpp:159-193`),
whose single friend is `TaskHost`.

### 2.1 `task::UiObservationSnapshot`

New header `modules/task/source/task/ui-observation.hpp` with
`ui-observation.cpp` beside it. It is a separate header rather than an addition
to `page-model-file.hpp` because an observation is not an artifact concept;
`page-model-file.hpp` already forward-declares across the boundary the same way.

```cpp
#pragma once

#include <core/error/result.hpp>
#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>
#include <domain/ids.hpp>

#include <string>

namespace uf::task
{
    class TaskHost;

    // One completed observation cycle, as the trusted Runtime resolved it.
    // Only the Host can mint one: C++ interprets no RuntimeModel field, and the
    // resolver runs behind the Host's private capability surface, so there is
    // no second producer to disagree with this one.
    //
    // It carries no frame, no native handle and no Receipt. It is what the
    // Operator may plan on, not what anything may act with; the exact-cycle
    // Receipt stays inside Host storage and dies with its cycle.
    class UiObservationSnapshot final
    {
        friend class TaskHost;

        std::string      m_observationId;
        GenerationId     m_generation;
        TargetGeneration m_targetGeneration;
        ContentHash      m_artifactRootHash;
        ContentHash      m_semanticHash;
        ContentHash      m_stateResolutionHash;
        std::string      m_canonicalJcs;

        UiObservationSnapshot(
            std::string observationId,
            GenerationId generation,
            TargetGeneration targetGeneration,
            ContentHash artifactRootHash,
            ContentHash semanticHash,
            ContentHash stateResolutionHash,
            std::string canonicalJcs
        );

    public:
        UiObservationSnapshot(UiObservationSnapshot const&) = default;
        UiObservationSnapshot(UiObservationSnapshot&&) noexcept = default;
        auto operator=(UiObservationSnapshot const&)
            -> UiObservationSnapshot& = default;
        auto operator=(UiObservationSnapshot&&) noexcept
            -> UiObservationSnapshot& = default;
        ~UiObservationSnapshot() = default;

        // Per-capture identity. It is diagnostic only, and deliberately not part
        // of any decision basis: two captures that resolved to the same state
        // must not force a caller to re-plan or re-approve.
        [[nodiscard]]
        auto observationId() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto generation() const noexcept -> GenerationId;
        [[nodiscard]] auto targetGeneration() const noexcept -> TargetGeneration;
        [[nodiscard]] auto artifactRootHash() const -> ContentHash;
        [[nodiscard]] auto semanticHash() const -> ContentHash;

        // sha256 of canonicalJcs(). Named for the schema member it becomes, and
        // it forwards rather than storing a second truth.
        [[nodiscard]] auto stateResolutionHash() const -> ContentHash;

        // The exact RFC 8785 JCS StateResolution document. This, and never
        // observationId(), is what reaches the plugin.
        [[nodiscard]]
        auto canonicalJcs() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;
    };
}
```

The mint is one new public member on `TaskHost`
(`modules/task/source/task/task-host.hpp`):

```cpp
        // Runs one observation cycle on a Runtime generation and returns what
        // the trusted resolver concluded. Annotation generations are refused:
        // the kinds never convert, and a production snapshot must not be able
        // to reach authoring files.
        [[nodiscard]]
        auto observe(GenerationId generation) -> Result<UiObservationSnapshot>;
```

§9 records the one thing that blocks a live implementation of it.

### 2.2 `operator_runtime::ProjectObservation`

New header `modules/operator/source/operator/project-observation.hpp` with
`project-observation.cpp` beside it, matching `journal-entry`,
`reconcile-outcome` and `tool-invocation`.

```cpp
#pragma once

#include "project-plugin.hpp"

#include <core/safety/annotations.hpp>
#include <core/types/integer.hpp>

#include <domain/content-hash.hpp>

#include <string>

namespace uf::operator_runtime
{
    class OperatorCoordinator;

    // The project's reading of one UI observation: the opaque payload
    // ProjectPlugin.derive returned, bound to the exact inputs it was derived
    // from. It is a state kind rather than a document because s01 gives it its
    // own revision line and exactly one writer, and the writer is derive.
    //
    // The three input members are separate values rather than one combined
    // hash, because a stale answer has to name the dimension that moved.
    class ProjectObservation final
    {
        friend class OperatorCoordinator;

        ContentHash       m_projectRegistrationHash;
        ContentHash       m_pluginHash;
        std::string       m_projectInstanceKey;
        ContentHash       m_stateResolutionHash;
        uint64            m_projectStateRevision;
        ContentHash       m_projectStateHash;
        uint64            m_revision;
        ValidatedDocument m_payload;

        ProjectObservation(
            ContentHash projectRegistrationHash,
            ContentHash pluginHash,
            std::string projectInstanceKey,
            ContentHash stateResolutionHash,
            uint64 projectStateRevision,
            ContentHash projectStateHash,
            uint64 revision,
            ValidatedDocument payload
        );

    public:
        ProjectObservation(ProjectObservation const&) = default;
        ProjectObservation(ProjectObservation&&) noexcept = default;
        auto operator=(ProjectObservation const&) -> ProjectObservation& = default;
        auto operator=(ProjectObservation&&) noexcept
            -> ProjectObservation& = default;
        ~ProjectObservation() = default;

        [[nodiscard]] auto projectRegistrationHash() const -> ContentHash;
        [[nodiscard]] auto pluginHash() const -> ContentHash;

        [[nodiscard]]
        auto projectInstanceKey() const noexcept UF_LIFETIME_BOUND
            -> std::string const&;

        [[nodiscard]] auto stateResolutionHash() const -> ContentHash;
        [[nodiscard]] auto projectStateRevision() const noexcept -> uint64;
        [[nodiscard]] auto projectStateHash() const -> ContentHash;

        [[nodiscard]] auto revision() const noexcept -> uint64;

        // project_observation_hash. Forwards to the payload's content hash;
        // there is no second digest to disagree with the bytes.
        [[nodiscard]] auto hash() const -> ContentHash;

        [[nodiscard]]
        auto payload() const noexcept UF_LIFETIME_BOUND
            -> ValidatedDocument const&;
    };
}
```

### 2.3 The minting path, and why a caller cannot forge one

Un-forgeability is in two layers, and both already exist as patterns:

1. **The payload.** `m_payload` is a `ValidatedDocument`, whose only constructor
   is private with `friend class ProjectSchemaOwner`
   (`project-plugin.hpp:61-88`). The only way to obtain one stamped
   `Derive`/`Output` is `ProjectPluginHandle::derive`, which validates against
   the exact `project_observation_schema` bytes the registration pinned —
   `ProjectSchemaOwner::create` hashes those bytes and compares them to
   `registration.projectObservationSchemaHash()` (`project-plugin.cpp:207-224`).
   A caller with arbitrary bytes cannot produce one.
2. **The binding.** `ProjectObservation`'s only constructor is private with a
   single friend, `OperatorCoordinator`. The revision, the ProjectState
   revision/hash and the state-resolution hash are read inside the snapshot
   transaction and are not parameters of any public function. A caller holding a
   legitimately derived `ValidatedDocument` still cannot claim it was derived
   against a ProjectState revision it was not.

Two consequences the implementer must respect:

- `friend class OperatorCoordinator` reaches `OperatorCoordinator`'s member
  functions only. It does not reach `OperatorCoordinator::Impl` or the
  file-local helpers in the anonymous namespace of `ledger.cpp`. The
  construction must happen in the body of `OperatorCoordinator::createSnapshot`,
  the way `commitReconciliation` keeps its reducer call in its own body.
- There is exactly one friend. Adding a second — a test accessor, a builder —
  reopens the hole; the falsification in §7 is what notices.

## 3. Atomic composition (`s02`)

### 3.1 What exists today

`OperatorCoordinator::createSnapshot` (`ledger.cpp:2529-2596`) opens a
transaction, re-reads the lease row and compares seven fields plus
`lease.sessionEpoch == m_impl->sessionEpoch`, mints a random token, and inserts
`(token, session_id, session_epoch, identity_hash, lease_revision)`. The
identity hash is the caller's second parameter, unexamined. Nothing about
ProjectState, the plugin, or the UI is read.

The join that later redeems the token is in `createOrLoadOperation`
(`ledger.cpp:2742-2764`):

```sql
SELECT session.controlled_target_key FROM snapshots s JOIN sessions session
ON session.session_id=s.session_id JOIN control_leases lease
ON lease.controlled_target_key=session.controlled_target_key
WHERE s.token=?1 AND s.session_id=?2 AND s.session_epoch=?3 AND
s.lease_revision=lease.revision AND lease.session_id=s.session_id
```

`s.lease_revision=lease.revision` is the only compare-and-swap the token
carries. A token therefore goes stale when the lease moves and at no other time.

### 3.2 The isolation, verified

`Transaction::begin` executes `BEGIN IMMEDIATE` (`ledger.cpp:324`), and the
destructor rolls back if `commit()` was not reached (`ledger.cpp:307-318`).
`open()` sets and reads back `journal_mode=WAL`, `foreign_keys=ON`,
`synchronous=FULL`, `trusted_schema=OFF`, failing if any read-back disagrees
(`ledger.cpp:406-448`), and `sqlite3_busy_timeout` is 5000 ms
(`ledger.cpp:401`).

`BEGIN IMMEDIATE` takes the write lock on the first statement rather than on the
first write. Every read inside the transaction therefore sees one committed
state, and no other connection can commit into the middle of it. That is the
whole isolation W3 needs; no new locking primitive is introduced.

Compare-and-swap is the repository's second mechanism and is used where a
revision must not have moved: `UPDATE ... AND revision=?9` followed by
`sqlite3_changes(...) != 1` (`ledger.cpp:3830-3881`, `ledger.cpp:3927-3953`),
and the epoch's startup CAS (`ledger.cpp:1022-1035`). W3 uses CAS in the join,
not in the insert: the snapshot row is new, so there is nothing to swap, and the
revisions it records are re-checked by the extended join in §3.4.

### 3.3 The exact sequence

`createSnapshot` becomes the composition. All of it is inside one
`Transaction::begin`.

1. `Transaction::begin(m_impl->database.get())` — `BEGIN IMMEDIATE`.
2. Read the lease row for `lease.controlledTargetKey` and compare all seven
   columns plus `lease.sessionEpoch == m_impl->sessionEpoch`. Unchanged from
   today (`ledger.cpp:2535-2571`); it is what `contract-control-c01` and
   `contract-control-c02` already falsify.
3. Read the session joined to its registration:
   `session.project_instance_key`, `session.manifest_hash`,
   `session.controlled_target_key`, `session.runtime_artifact_root_hash`,
   `session.installed_generation`, `registration.plugin_id`,
   `registration.plugin_hash`, `session.project_registration_hash`, with
   `session.session_id=lease.sessionId AND session.active=1 AND
   session.session_epoch=?`. Refuse otherwise.
4. Refuse unless `plugin.pluginId()`, `plugin.pluginHash()` and
   `plugin.projectRegistrationHash()` all equal the session's three columns —
   the same three-way check `commitReconciliation` makes
   (`ledger.cpp:3604-3614`).
5. Refuse unless `observation.artifactRootHash()` equals
   `session.runtime_artifact_root_hash`. Without it a snapshot can be composed
   from a UI observation taken through a RuntimeArtifact the session never
   pinned, and `session_manifest_hash` would attest to a model that produced
   none of the evidence.
6. Read `project_state` for `(plugin_id, project_instance_key)`: `revision`,
   `state_hash`, `canonical_state`, `project_registration_hash`,
   `state_schema_hash`. Refuse if absent — the row is created by
   `provisionProjectInstance`, so its absence is an invariant failure, not a
   caller error.
7. Read the latest `project_observations` row for the same key, ordered by
   `revision DESC LIMIT 1`. It supplies `prior_project_observation` and the
   revision comparison in step 11. Absent on the first snapshot of an instance.
8. Read the non-terminal Operation for `(plugin_id, project_instance_key)`, if
   any, as `operation_id`, `state`, `revision`. It is the design's
   `pending_operation_transition` and it is a read-only summary.
9. Assemble the derive envelope **in the Operator**, byte for byte, exactly the
   way `reduceEnvelopeJcs` assembles the reducer's input
   (`ledger.cpp:840-883`), and for the reason stated there: a caller that
   supplied the input could have the record say one thing and the derivation see
   another. JCS orders members by UTF-16 code unit, giving:

   ```text
   {"pending_operation_transition":<null | {"operation_id":…,"revision":…,"state":…}>,
    "pinned_project_artifact_identities":[<root hash>, …],
    "prior_project_observation":<null | canonical observation payload>,
    "project_state":<project_state.canonical_state bytes>,
    "ui_snapshot":<observation.canonicalJcs()>}
   ```

   Rules that are not optional:
   - the two optional members carry the literal `null`, never absence, for the
     reason `reduceEnvelopeJcs` already gives: a plugin must distinguish "no
     prior" from "a prior I failed to read";
   - `pinned_project_artifact_identities` is in registration order, which the
     manifest rule already fixes as artifact-root names sorted by UTF-8 bytes,
     so the array order is determined and JCS does not reorder arrays;
   - `ui_snapshot` carries the canonical StateResolution document and never
     `observation.observationId()`. This is what makes a semantically equivalent
     recapture produce an identical derive input, an identical
     `project_observation_hash` and an identical `decision_basis_hash`, which is
     `s04`'s "semantically equivalent new capture does not force re-approval";
   - `ui_snapshot` is the design's own member name for the derive input, while
     the snapshot document's member is `ui_observation`. Both spellings are the
     authority's, for two different objects. §9 records that.
10. `plugin.canonicalize(envelope)` then `plugin.derive(input)`. The plugin runs
    **inside** the transaction, as `reduce` already does
    (`ledger.cpp:3710-3739`), and the comment there is the justification: the
    read of its inputs and the derivation from them must be one
    `BEGIN IMMEDIATE`, or a concurrent writer moves the state between the two.
    The plugin VM is quota-bound, so holding the write lock across it is
    bounded.
11. Refuse unless the returned document is `Derive`/`Output` and its
    `projectRegistrationHash()` matches the session's.
12. Compute the next observation revision. Let the *derive fingerprint* be the
    tuple `(state_resolution_hash, project_state_revision, project_state_hash,
    plugin_hash, project_registration_hash, derived payload hash)`. If step 7
    found a row whose fingerprint is equal, reuse its `revision`; otherwise
    `revision + 1`, starting at 1. This is the design's "任一 UI/artifact/plugin/
    project-state 输入变化" made executable, and it is what keeps a re-observed
    but unchanged world on one revision.
13. Insert the `project_observations` row when the revision is new. Mint the
    `ProjectObservation` value here, in the member function body.
14. Compute `decision_basis_hash` = sha256 of the canonical `DecisionBasis`
    material the design names — `state_resolution_hash + project_observation_hash
    + project_state_hash + session_manifest_hash` — serialized as JCS so the
    concatenation is unambiguous:

    ```text
    {"project_observation_hash":…,"project_state_hash":…,
     "session_manifest_hash":…,"state_resolution_hash":…}
    ```
15. Read `availability_revision` as
    `SELECT COALESCE(MAX(sequence), 0) FROM control_transitions WHERE
    controlled_target_key=?1`. It is Operator-owned, monotonic, and moves on
    acquire/takeover/release, which is three of the four triggers the design
    lists for `ControlState`. Policy is the fourth and has no store yet (§9).
16. Allocate `snapshot_revision` as the session's monotonic snapshot counter:
    `COALESCE(MAX(snapshot_revision), 0) + 1` for this `session_id`.
17. Compute `identity_hash` = sha256 of the canonical `SnapshotParts` JCS, its
    15 members in JCS order:

    ```text
    availability_revision, controlled_target_id, decision_basis_hash,
    fencing_token, lease_id, observation_id, project_instance_key,
    project_observation_hash, project_observation_revision, project_state_hash,
    project_state_revision, session_epoch, session_manifest_hash,
    state_resolution_hash, target_generation
    ```
18. `randomToken(...)` as today, then insert the `snapshots` row with the derived
    identity and the CAS columns of §5.
19. `transaction.commit()`.

### 3.4 The join that redeems the token

`createOrLoadOperation`'s snapshot query gains the dimensions the snapshot now
records. The existing `s.lease_revision=lease.revision` stays; two clauses join
it:

```sql
SELECT session.controlled_target_key FROM snapshots s
JOIN sessions session ON session.session_id=s.session_id
JOIN control_leases lease ON lease.controlled_target_key=session.controlled_target_key
JOIN project_state state ON state.plugin_id=s.plugin_id
  AND state.project_instance_key=s.project_instance_key
JOIN project_observations obs ON obs.plugin_id=s.plugin_id
  AND obs.project_instance_key=s.project_instance_key
  AND obs.revision=s.project_observation_revision
WHERE s.token=?1 AND s.session_id=?2 AND s.session_epoch=?3
  AND s.lease_revision=lease.revision AND lease.session_id=s.session_id
  AND s.project_state_revision=state.revision
  AND obs.project_state_revision=state.revision
```

The last two clauses are the compare-and-swap that makes the token a reference
to a *composition* rather than to a lease. `obs.project_state_revision=state.revision`
is not redundant with the clause above it: it refuses a snapshot whose recorded
observation revision was reused from an earlier ProjectState, which is the only
way the reuse rule in step 12 could otherwise smuggle a stale reading forward.

### 3.5 What a torn snapshot looks like, and what prevents it

Without the composition, the sequence a caller must perform is: observe, read
ProjectState, call `derive`, compute an identity hash, call `createSnapshot`.
Between the ProjectState read and the derive call, a `commitReconciliation` on
another connection advances the state from R to R+1. The caller then presents an
identity hash naming revision R while the derived observation was computed
against R+1 — or the reverse. The snapshot attests to a world that was never
true at any instant, and an Operation planned on it freezes a plan whose
preconditions were never simultaneously satisfied. `decision_basis_hash` then
certifies the same fiction, and every downstream stale check agrees with it,
because they all compare against the recorded basis.

Three things prevent it, and all three are necessary:

1. Only the coordinator reads ProjectState and only the coordinator calls
   `derive`; neither is a parameter, so there is nothing for a caller to skew.
2. Both happen inside the one `BEGIN IMMEDIATE` that also inserts the snapshot
   row, so no other connection can commit between them.
3. The identity is computed from what that transaction read (§4), so there is no
   caller-supplied value left to disagree with it.

One window remains and is intended. The capture itself happens before the
transaction, because it involves the screen. The snapshot therefore attests
"this UI observation, as read against this ProjectState revision" — the
observation-to-state join is atomic, the capture instant is not. The design
accepts that explicitly: the SnapshotToken is a reference to planning-time
facts, and `execute` must re-observe after taking the target mutation gate. W3
records `target_generation` so the delivery-time check has something to compare
against; making that comparison is W4's.

## 4. Derived snapshot identity

Today the identity is the second parameter of `createSnapshot` and is written to
the row unexamined (`ledger.cpp:2585`). After W3 it is sha256 of the canonical
`SnapshotParts` JCS listed in §3.3 step 17, every member of which the
transaction read from an owner:

| Member | Source |
|---|---|
| `session_epoch` | `m_impl->sessionEpoch`, matched against the lease and the session row |
| `controlled_target_id` | `session.controlled_target_key` |
| `target_generation` | `observation.targetGeneration()` |
| `observation_id` | `observation.observationId()` |
| `state_resolution_hash` | `observation.stateResolutionHash()` |
| `project_observation_revision` | step 12 |
| `project_observation_hash` | the derived document's content hash |
| `project_instance_key` | `session.project_instance_key` |
| `project_state_revision`, `project_state_hash` | the `project_state` row |
| `decision_basis_hash` | step 14 |
| `session_manifest_hash` | `session.manifest_hash` |
| `availability_revision` | step 15 |
| `lease_id`, `fencing_token` | the verified lease row |

**Exactly one parameter disappears: `ContentHash const& identityHash`.** Nothing
replaces it. `ControlLease const& lease` stays; two parameters are added, and
both are values a caller cannot fabricate — a `ProjectPluginHandle` obtainable
only from `ProjectPluginRegistrar::findExact`, and a `UiObservationSnapshot`
mintable only by `TaskHost`.

Two properties follow and are tested in §7:

- two snapshots over identical composed state carry equal `identityHash` and
  different `token` and `snapshotRevision`, because `token_id`, `session_id` and
  `snapshot_revision` are `SnapshotIdentity` members and not `SnapshotParts`
  members;
- a snapshot taken after any one composed dimension moved carries a different
  `identityHash`, because that dimension is a member of the hashed parts.

## 5. DDL

Two changes, both inside the existing `BEGIN IMMEDIATE ... COMMIT` block at
`ledger.cpp:502-768`, placed in the block so the table order stays readable.

One new table, after `project_instances`:

```sql
CREATE TABLE IF NOT EXISTS project_observations(
    plugin_id TEXT NOT NULL,
    project_instance_key TEXT NOT NULL,
    revision INTEGER NOT NULL CHECK(revision > 0),
    project_registration_hash TEXT NOT NULL
        REFERENCES project_registrations(registration_hash),
    plugin_hash TEXT NOT NULL,
    observation_schema_hash TEXT NOT NULL,
    state_resolution_hash TEXT NOT NULL,
    project_state_revision INTEGER NOT NULL CHECK(project_state_revision >= 0),
    project_state_hash TEXT NOT NULL,
    canonical_observation TEXT NOT NULL,
    observation_hash TEXT NOT NULL,
    FOREIGN KEY(plugin_id, project_instance_key)
        REFERENCES project_instances(plugin_id, project_instance_key),
    PRIMARY KEY(plugin_id, project_instance_key, revision)
) STRICT;
```

The `snapshots` table replaces its current five columns
(`ledger.cpp:625-631`) with:

```sql
CREATE TABLE IF NOT EXISTS snapshots(
    token TEXT PRIMARY KEY,
    session_id TEXT NOT NULL REFERENCES sessions(session_id),
    snapshot_revision INTEGER NOT NULL CHECK(snapshot_revision > 0),
    session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
    identity_hash TEXT NOT NULL,
    lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),
    plugin_id TEXT NOT NULL,
    project_instance_key TEXT NOT NULL,
    observation_id TEXT NOT NULL,
    target_generation INTEGER NOT NULL CHECK(target_generation > 0),
    state_resolution_hash TEXT NOT NULL,
    project_observation_revision INTEGER NOT NULL
        CHECK(project_observation_revision > 0),
    project_state_revision INTEGER NOT NULL CHECK(project_state_revision >= 0),
    decision_basis_hash TEXT NOT NULL,
    availability_revision INTEGER NOT NULL CHECK(availability_revision >= 0),
    UNIQUE(session_id, snapshot_revision),
    FOREIGN KEY(plugin_id, project_instance_key, project_observation_revision)
        REFERENCES project_observations(plugin_id, project_instance_key, revision)
) STRICT;
```

The `snapshots` table is declared after `project_observations` so the foreign
key resolves; `operations.snapshot_token` already references `snapshots(token)`
and is unaffected.

### 5.1 The two constants that must be recomputed

**There is no named constant for the schema fingerprint.** It is an inline
string literal inside `verifyExactDatabaseSchema`, at
`modules/operator/source/operator/ledger.cpp:368-373`:

```cpp
            UF_TRY_VALUE(
                expected,
                ContentHash::parse(
                    "sha256:5738e6f98534efbdfc3114413de70c032b64e2cbaa84d4c152ec6cbb512120a4"
                )
            );
```

`rg` over the tree finds it in that file and in `build/` artifacts only; nothing
in `schema/`, `tests/` or `scripts/` pins it, and `tests/test-runtime-surface.py`
does not carry it — its `SCHEMA_AUTHORITIES` tuple pins four *file* hashes
(`k_traceSchemaHash`, `k_runtimeArtifactSchemaHash`, `k_runtimeModelSchemaHash`,
`k_annotationWorkspaceSchemaHash`) and none of them is this one.

The value is sha256 over the canonicalization the function builds: for each
`sqlite_schema` row where `name NOT LIKE 'sqlite_%'`, ordered by `type, name`,
the four columns `type, name, tbl_name, coalesce(sql, '')` each written as
`<byte length>:<value>`. Recompute it by creating a database with the new DDL
and applying the same canonicalization — a throwaway script outside the tree, or
temporary `[DEBUG-schema]` instrumentation in that function, removed before the
change is complete.

**The second constant is `expectedTables`** at `ledger.cpp:476-482`, a
comma-joined sorted table name list. It gains `project_observations` between
`project_instances` and `project_registrations`:

```cpp
                constexpr auto expectedTables = std::string_view{
                    "approvals,authority_decisions,control_leases,control_transitions,"
                    "dispatches,fencing_high_water,journal_events,operations,"
                    "project_instances,project_observations,project_registrations,"
                    "project_state,reconciliations,runtime_artifacts,"
                    "runtime_installations,runtime_publications,runtime_state,"
                    "sessions,snapshots"
                };
```

### 5.2 `user_version` stays at 1

`PRAGMA user_version=1` (`ledger.cpp:765`) does not change, and the
`applicationId != applicationIdentity || userVersion != 1U` gate
(`ledger.cpp:462`) does not gain a second accepted value. There is exactly one
Operator schema; bumping the version would create a second spelling of "the
schema" with no reader, which is the compatibility path CLAUDE.md forbids. A
pre-W3 database still refuses precisely: its table set no longer matches, so
`open()` fails at `ledger.cpp:483-489` with "Operator database table set does
not match schema v1" before the fingerprint is even consulted. No data
migration is owed, because nothing outside a temporary test directory holds an
`operator-runtime.sqlite`.

## 6. Signature changes

| Symbol | Before | After |
|---|---|---|
| `OperatorCoordinator::createSnapshot`<br>`ledger.hpp:253-257` | `(ControlLease const&, ContentHash const& identityHash) -> Result<SnapshotRecord>` | `(ControlLease const&, ProjectPluginHandle const&, task::UiObservationSnapshot const&) -> Result<SnapshotRecord>` |
| `SnapshotRecord`<br>`ledger.hpp:79-86` | `token, sessionId, identityHash, sessionEpoch, leaseRevision` | those five, plus `snapshotRevision`, `stateResolutionHash`, `projectStateRevision`, `projectStateHash`, `decisionBasisHash`, `availabilityRevision`, and `ProjectObservation observation` |
| `OperatorCoordinator::createOrLoadOperation` | signature unchanged | its snapshot join gains the two clauses in §3.4 |
| `TaskHost::observe` | does not exist | `(GenerationId) -> Result<UiObservationSnapshot>`, public |
| `task::UiObservationSnapshot` | does not exist | new header `modules/task/source/task/ui-observation.hpp` |
| `operator_runtime::ProjectObservation` | does not exist | new header `modules/operator/source/operator/project-observation.hpp` |

`ledger.hpp` gains `#include <task/ui-observation.hpp>` and
`#include "project-observation.hpp"`. `modules/operator/manifest.txt` already
declares `public = core domain task`, so no dependency edge changes and
`check_modules.py` is unaffected.

`SnapshotRecord` holding a `ProjectObservation` makes it non-default-constructible.
That is already true of the struct — `ContentHash identityHash;` has no in-class
initializer because `ContentHash` has no default constructor — and every
construction site is an aggregate initializer, so nothing else changes.

### 6.1 Every call site

Production:

- `modules/operator/source/operator/ledger.cpp:2529` — the definition.
- `modules/operator/source/operator/ledger.cpp:2742-2764` — the join.
- `modules/operator/source/operator/ledger.cpp:625-631`, `476-482`, `368-373` —
  DDL, table list, fingerprint.

Tests and the exported suite:

- `tests/operator/project-fixture.hpp:770` — `prepareStore` must first obtain a
  `UiObservationSnapshot` and pass the plugin handle it already holds.
- `tests/operator/project-fixture.hpp:194-284` — the fixture's
  `CanonicalJsonValidator` is an exact-string allowlist and its
  `ProjectDocumentValidator` accepts `"{}"` for `Derive`/`Input`
  (line 253). Both refuse the real derive envelope. Add a
  `looksLikeDeriveEnvelope` predicate beside the existing
  `looksLikeReduceEnvelope`, and record the bytes the way `lastReduceInput`
  records the reducer's — the §7 tests read it.
- `tests/operator/project-fixture.hpp:653` — the fixture plugin's
  `derive = function(_input) return '{}' end` still returns `{}`; the
  `Derive`/`Output` branch at line 271 already accepts it, so the output side
  needs no change.
- `tests/operator/test-control-contract.cpp:155`, `181`, `211`.
- `tests/operator/test-ledger.cpp:243` (that file has its own `prepareStore`),
  `621`.
- `contract-suite/source/harness.cpp:258`.
- `contract-suite/source/suite-control-ledger.cpp:37`.
- `contract-suite/fixtures/arcana-expedition/provider.cpp:371-397` — the second
  fixture's `Derive`/`Input` validator accepts one exact string and needs the
  same envelope predicate. Its `Derive`/`Output` branch is unaffected.
- `contract-suite/fixtures/umbraflow/provider.cpp` adapts
  `tests/operator/project-fixture.hpp` and needs no edit of its own.
- `contract-suite/include/operator-contract/project-under-test.hpp` — add
  `std::shared_ptr<std::string> observedDeriveInput;` beside
  `observedReduceInput`, with the same single-threaded contract. A consuming
  repository must supply it, so this is a real break of the exported surface and
  belongs in the commit message.

## 7. Falsifiable tests

Every case below states its property and the exact single-line mutation that
must turn it red. A test whose named mutation leaves it green is a defect, not a
test. **The implementer must actually apply each mutation, observe the failure,
and revert it**; a negative result proves nothing until the experiment is shown
able to produce a positive one.

`contract-state-s01` and `contract-state-s02` are already registered in
`tests/CMakeLists.txt` (`UF_REQUIRED_DOCTEST_CONTRACTS`, and the `CASES` list of
`test-contract-operator`). W3 turns both into behavioural cases and moves their
current schema-shape assertions into new `schema-state-s01` and
`schema-state-s02` cases, which is W10's rename applied to these two IDs.
Registering new IDs requires updating
[`2026-08-09-runtime-migration-report.md`](2026-08-09-runtime-migration-report.md)
**first** — stop condition 2 of that report is "a schema path or test ID changes
without updating this report first".

### `contract-state-s01`

**T1 — project state cannot be written back as UI evidence.**
Property: the derive envelope carries the Host's observation in `ui_snapshot`
and the database's ProjectState in `project_state`, in separate members, and no
caller supplies either. Assert on the recorded derive input that `ui_snapshot`
equals `observation.canonicalJcs()` and `project_state` equals the
`canonical_state` bytes the row holds.
Mutation: in the envelope builder, change
`envelope += observation.canonicalJcs();` to
`envelope += projectStateJcs;`.

**T2 — the ProjectObservation revision moves only when its input moved.**
Property: two `createSnapshot` calls with the same `UiObservationSnapshot` and
an unchanged ProjectState return the same `observation.revision()` and the same
`observation.hash()`; a third after a committed reconciliation returns
`revision + 1`.
Mutation A: change the revision line to `auto const nextRevision = storedRevision + 1U;`
(unconditional). Red on the first half.
Mutation B: change it to `auto const nextRevision = storedRevision;`
(never advances). Red on the second half.

**T3 — the derived reading is bound to the registration that produced it.**
Property: `createSnapshot` refuses a `ProjectPluginHandle` for a different
registration, even when that handle is itself valid. Use the `Foreign` project
role the contract suite already provides.
Mutation: delete the line
`|| plugin.projectRegistrationHash().hex() != columnText(sessionQuery.get(), N)`
from the step-4 check.

**T4 — the observation cannot come through another RuntimeArtifact.**
Property: a `UiObservationSnapshot` whose `artifactRootHash()` is not the
session's pinned root is refused. Costs a second installed artifact in the
fixture.
Mutation: delete the step-5 comparison line.

### `contract-state-s02`

**T5 — the identity is derived from the composition, not accepted.**
Property, as five assertions in one case: (a) two snapshots over identical
composed state have equal `identityHash` and different `token` and
`snapshotRevision`; (b) after a committed reconciliation the `identityHash`
differs; (c) with a different `stateResolutionHash` it differs; (d) with a
different `session_manifest_hash` — a second session pinned to another manifest
— it differs; (e) a recapture with a new `observationId` but the same
`stateResolutionHash` and unchanged ProjectState leaves `identityHash` and
`decisionBasisHash` equal.
Mutations, one line each in the parts builder:
- drop the `project_state_hash` member → red on (b);
- drop the `state_resolution_hash` member → red on (c);
- drop the `session_manifest_hash` member → red on (d);
- add `observation_id` to the `decision_basis_hash` material → red on (e);
- add `token` to the parts material → red on (a).

**T6 — a token goes stale when ProjectState moves, not only when the lease does.**
Property: a token that opened an Operation before a reconciliation is refused
after it, and a token taken after it is accepted. This is the join of §3.4.
Mutation: delete `AND s.project_state_revision=state.revision` from the snapshot
query. Red.
Second mutation, same case: delete
`AND obs.project_state_revision=state.revision`, and assert the reuse path — a
snapshot, a reconciliation, a snapshot whose observation payload is byte-identical
— still refuses the first token. Red.

**T7 — the composition is one transaction.**
Property: the ProjectState bytes the plugin saw are the bytes the snapshot names.
Assert that the recorded derive input's `project_state` member hashes to
`record.projectStateHash` and that
`record.observation.projectStateRevision() == record.projectStateRevision`.
Mutation: bind the snapshot row's `project_state_revision` from a fresh
`SELECT revision FROM project_state ...` issued after `transaction.commit()`
rather than from the in-transaction local. Red, and it is the one-line form of
the tear §3.5 describes.

**T8 — `createSnapshot` has no identity parameter.**
Property: compile-time. It is not a doctest case; it is the
`contract-repository-surface` gate's job. Add a rule to
`tests/test-runtime-surface.py` that `createSnapshot` must not name a parameter
whose identifier ends in `identityHash` or `IdentityHash`.
Mutation: reintroduce the parameter in the declaration. Red.

### `schema-state-s01`, `schema-state-s02`

The current bodies of `contract-state-s01` and `contract-state-s02`
(`tests/operator/test-state-contract.cpp:165-191`) move here unchanged. They
assert schema shape and nothing else, which is exactly what their new names
claim.

## 8. Assumptions about W2

W2 is being specified in parallel into
`docs/plans/2026-08-10-w2-effective-plan.md`, which does not exist at the time
of writing. Each assumption below is stated so it can be checked rather than
discovered.

1. **W2 owns `EffectivePlan`; W3 mints no plan.** `createSnapshot` produces no
   `frozen_plan_hash`, no `step_intent_hash` and no `effect_envelope_hash`.
2. **`reserveDispatch` is untouched by W3.** It keeps its current
   `decisionBasisHash`, `frozenPlanHash`, `stepIntentHash` caller parameters
   (`ledger.hpp:275-285`) until W2 replaces them with the minted plan. If W2
   lands first, W3 rebases onto the new signature and changes nothing else.
3. **`decision_basis_hash` is minted here and consumed there.** W3 computes it
   in the snapshot transaction from the four hashes the design names, and
   records it on the snapshot row. W2 is assumed to read it from the snapshot
   rather than recompute it. If W2 computes it inside the plan mint, one of the
   two must drop it — two producers of one hash is the drift `s04` exists to
   prevent.
4. **W2's `plan()` input comes from the coordinator, not the caller.** W3
   exposes `ProjectObservation` on `SnapshotRecord` for exactly that, so that
   the `project_snapshot` argument to `plan` can be assembled from
   Operator-held values. If W2 instead assembles it from a caller-supplied
   document, `s02` is reopened at the plan boundary.
5. **A snapshot is per step, and reusable.** W2's bounded step sequencing
   re-observes before each step, which is assumed to mean a fresh
   `createSnapshot` per step. `createSnapshot` therefore stays cheap enough to
   run per step and stays non-consuming: `contract-state-s03` already asserts
   one token opens several Operations, and W3 preserves that.
6. **Any W2 column on `snapshots` is merged, not stacked.** The two DDL changes
   must be applied as one union and the schema fingerprint recomputed once, not
   twice. Whichever item lands second recomputes it. §8.1 records what W2
   actually proposes.
7. **`available_tools` is W2's or W6's, not W3's.** W3 populates
   `availability_revision` from the control-transition high-water (§3.3 step 15)
   and does not compute an available-tool set. The moment W2 or W6 introduces
   one, `availability_revision` must become that set's revision instead, and the
   snapshot identity changes shape with it.
8. **W4 consumes `target_generation` from the snapshot row.** W3 records it; the
   delivery-time comparison against the Host's current generation is W4's.

### 8.1 Divergences observed after W2's draft appeared

`docs/plans/2026-08-10-w2-effective-plan.md` was written into the tree while
this specification was being written. It was read, not edited. It changes
`createSnapshot` too, so the following four points are the real reconciliation
list. None of them is settled here.

1. **`ObservedSnapshotParts` disappears entirely, not partly.** W2 §5 introduces
   a caller-supplied `ObservedSnapshotParts{observationId, stateResolutionHash,
   projectObservationHash, targetGeneration, projectObservationRevision,
   availabilityRevision}` and says "W3 removes the two hashes below as well".
   W3 removes all six: the first, second and fourth come from the Host-minted
   `UiObservationSnapshot`, the third and fifth from `derive` inside the
   transaction, and the sixth from `control_transitions`. After W3 there is no
   caller-supplied parts struct, so `ObservedSnapshotParts` should be treated as
   a W2-only intermediate that W3 deletes — or skipped, if W3 lands close
   enough behind W2 that introducing it costs more than it buys.
2. **The `snapshots` columns must be unioned.** W2 adds `decision_basis_hash`
   and `canonical_parts`; W3 adds `snapshot_revision`, `plugin_id`,
   `project_instance_key`, `observation_id`, `target_generation`,
   `state_resolution_hash`, `project_observation_revision`,
   `project_state_revision`, `availability_revision` and the composite foreign
   key. `decision_basis_hash` is common to both. Assumption 6 above applies.
3. **Keep `canonical_parts` and the join columns.** W2's `canonical_parts` — the
   exact `SnapshotParts` JCS, following `journal_events.canonical_event` — is
   the better way to falsify the derivation, and this specification adopts it.
   It does not replace the scalar columns: §3.4 joins the token against
   `project_state.revision` and `project_observations.revision`, and SQL cannot
   join through a JSON text column. Both are needed, and the invariant that
   each scalar equals its member in `canonical_parts` is worth one test.
4. **Who "owns" the two hashes is a sentence, not a code split.** W2 §5 proposes
   "W2 derives `identity_hash` and `decision_basis_hash` from the parts; W3
   derives the parts", and flags it as inferred rather than stated. Both
   documents in fact compute both hashes in the body of `createSnapshot`, so
   there is no code conflict — only a wording decision about which plan gets
   credit for `s04`'s hash. Confirm it once and write it in one place.

## 9. Open questions

1. **The trusted resolver produces no canonical document.**
   `modules/task/runtime/resolution.luau` returns frozen Luau tables, and a
   search for `json` or `encode` across `modules/task/runtime/` finds nothing —
   `jsonl.luau` was deleted in the runtime rewrite. So `UiObservationSnapshot::canonicalJcs()`
   has no producer today, and `TaskHost::observe` cannot be implemented live
   until one exists. Two ways forward, and this specification does not choose:
   serialize inside the trusted Luau resolver, or serialize in the Host from the
   resolver's returned table. The second means C++ walking RuntimeModel-derived
   structure, which `tests/test-runtime-surface.py` treats as a violation
   ("C++ registers/interprets RuntimeModel semantics"), so it is probably the
   first. Either way it is a named prerequisite, not something W3 can assume.
2. **`controlled_target_id` versus `controlled_target_key`.** The schema's
   `SnapshotParts` member is `controlled_target_id`; every table and struct in
   the code spells it `controlled_target_key`. That is two spellings of one
   thing and it predates W3. Whichever survives must be applied to the schema
   and the DDL in one change; this specification writes the schema's member name
   over the code's value and does not pretend that is settled.
3. **`ui_snapshot` versus `ui_observation`.** The frozen design names the derive
   input member `ui_snapshot` and the `ProjectSnapshot` member `ui_observation`.
   They are different objects, so both are the authority's; a reader will still
   ask. Recorded, not resolved.
4. **`ProjectSnapshot` cannot be produced yet.** The schema requires
   `available_tools` and `event_cursor`, which have no owner until W6 and W7.
   W3 therefore produces a snapshot *record* — identity, token, and the three
   composed parts — and never a `ProjectSnapshot` wire document. The `s02` gate
   must not assert on one.
5. **`project_observations` grows without bound.** One row per distinct
   composition per project instance, forever. W8 is artifact GC by database
   refcount and covers `runtime-artifacts/` only. Whether observation rows are
   pruned, and by what rule that does not break the join in §3.4, has no owner.
6. **Policy has no store, so `availability_revision` is incomplete.** The
   design's `ControlState` moves on "acquire/takeover/policy/availability";
   §3.3 step 15 covers the first two. A policy change would not move the
   snapshot identity today. That is `c12`'s gap, surfaced here because the
   identity hash is where it becomes visible.
7. **Cost.** [`2026-08-10-next-block.md`](2026-08-10-next-block.md) budgets W3 at
   4 days. That estimate does not include item 1 above, and item 1 is on the
   critical path for a live `TaskHost::observe`. The Operator half — the value
   types, the composition, the DDL, the join, the tests — is separable and fits
   the estimate.
