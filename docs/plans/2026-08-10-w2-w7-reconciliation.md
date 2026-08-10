# W2-W7 reconciliation: read this before touching code

Status: ruling record. No code changed on its account.
Date: 2026-08-10
Scope: `umbraflow-cpp` only. No consumer-project writes.
Reconciles: [W2](2026-08-10-w2-effective-plan.md), [W3](2026-08-10-w3-snapshot-coordinator.md),
[W4](2026-08-10-w4-delivery-join.md), [W6/W7](2026-08-10-w6-w7-controller-and-agent.md).

Authority above this document:
[`2026-08-09-runtime-hardening-rewrite.md`](2026-08-09-runtime-hardening-rewrite.md)
and the frozen v1.9 bundle it pins.
Ordering and requirement state: [`2026-08-10-next-block.md`](2026-08-10-next-block.md).

The four specifications were written the same day by four agents who could not
see each other's output. Each is correct about its own item and each states
assumptions about the others that nobody checked. This document is the check.

**Where one of the four contradicts this document, this document wins.** The
four are not rewritten: an implementer reads this first, then the item's own
specification for detail.

Every file-level claim below was read out of the tree on 2026-08-10 at branch
`design/annotation-system-v2`. Other agents were editing the same tree while it
was written — `tests/CMakeLists.txt`, `tests/operator/*` and `contract-suite/`
all moved during the reading pass. §8 says exactly what could not be pinned.
Line numbers are anchors, not addresses.

## 1. The tree moved under all four documents

Three facts invalidate call-site tables in all four specifications. None of them
changes a design decision; all of them change what an implementer will find.

### 1.1 `tests/operator/project-fixture.hpp` no longer exists

It is `contract-suite/fixtures/umbraflow/project-fixture.hpp`, 867 lines, reached
by `target_include_directories(... "${UF_OPERATOR_FIXTURE_DIR}")` in
`tests/CMakeLists.txt`, so every `#include "project-fixture.hpp"` still spells the
same thing. W2 §8, W3 §6.1, W4 §5.4 and W6/W7 §7.2 all name the old path.

The line numbers those documents give are the *new* file's — the fixture plugin's
`derive`/`plan`/`next_step` stubs really are at 653-655, and the
`Derive`/`Plan`/`NextStep` document validator really does accept only `"{}"` on
both the input and output branches. So the content claims hold and only the paths
are stale.

Consequence for all four: an edit to the fixture is an edit to the **exported**
contract suite, which a consuming repository compiles. It belongs in the commit
message as a break of the consumer-visible surface, not as a test tweak.

### 1.2 The `contract-*` gates these items planned to rewrite are now `schema-*`

W10's rename landed ahead of W2-W7, verified by a from-scratch configure, build
and `ctest` run (68 of 68 passing). Twelve IDs moved:
`a01 a02 a03 a05 a07` → `schema-agent-*`, `c03 c05 c08` → `schema-control-*`,
`p01 p02 p03` → `schema-product-*`, `s01 s02 s04` → `schema-state-*`. A search of
the tree outside `docs/` finds **no** `contract-` spelling of any of those twelve.

**The gate total is no longer 42, and that is the correct outcome.** There are 47
gates over 42 requirements: 28 `contract-*` (label `CI;CONTRACT`) and 19
`schema-*` (label `CI;SCHEMA`). `c09` through `c13` carry two each — a
`schema-control-cNN` in `tests/operator/test-control-contract.cpp` for the schema
symbol the migration report defines that ID by, and a `contract-control-cNN` in
the exported suite for the store behaviour, now running from
`contract-suite-umbraflow`. One requirement owning two gates that guard different
things is exactly what R6 permits; what the vocabulary forbids is a name that
overstates what its case proves. A reader who sees "47 gates, 42 requirements"
and assumes an arithmetic slip will try to correct it, so the two counts are
stated together wherever either appears.

So the plan every item wrote for its own twelve IDs — "the case exists; replace
its body, append the schema assertions" — is not available. What each item must
do instead:

1. add a **new** `contract-<area>-<id>` `TEST_CASE`;
2. leave the `schema-<area>-<id>` case alone (R6: the split is per assertion);
3. add the new ID to `UF_REQUIRED_DOCTEST_CONTRACTS` **and** to one suite's
   `CASES`, because `cpp_add_contract_suite` requires the requested set and the
   declared set to be identical, and `tests/CMakeLists.txt` ends by requiring the
   registered set and the required set to be identical;
4. update [`2026-08-09-runtime-migration-report.md`](2026-08-09-runtime-migration-report.md)
   **first** — stop condition 2 is "a schema path or test ID changes without
   updating this report first", and the registry rejects an ID the report does
   not name.

W2 §8's "`tests/CMakeLists.txt` — no change" is therefore false, and so is W3
§7's "already registered in `tests/CMakeLists.txt`".

A second consequence W2 did not see: the behavioural halves of `c09`-`c13` now
live in `contract-suite/source/suite-control-ledger.cpp`, not in
`tests/operator/test-control-contract.cpp`. W2's T14 ("`contract-control-c12`
must be rewritten to take its hashes from real mints") targets a case in the
exported suite. That case is parameterised by `ProjectUnderTest`, so W2's new
`ProjectVocabulary` members are a break of
`contract-suite/include/operator-contract/project-under-test.hpp` that both
fixtures must satisfy.

### 1.3 Seven compiled cases run in no gate

`test-contract-operator` compiles 39 doctest cases and 32 are registered as
CTests. The other **seven run in nothing**: they are the `ProjectPlugin …` cases
in `tests/operator/test-project-plugin-contract.cpp` (lines 287, 332, 379, 413,
497, 557, 676 — the eighth case in that file is `contract-product-p05-fixtures`,
which is registered). They compile, they link, and no `ctest` invocation ever
executes them.

The cause is an asymmetry between two CMake helpers.
`cpp_add_contract_suite` calls `cpp_add_test(NO_CTEST ...)` and then registers one
CTest per `CASES` entry and nothing else, while `uf_add_operator_contract_suite`
additionally registers a `contract-suite-<project>` aggregate that runs the whole
binary. A case whose name is outside the `contract-`/`schema-` vocabulary is
therefore reachable in the exported suite and unreachable in the in-repo one.

**Ruled: `cpp_add_contract_suite` gains the aggregate, the same way the exported
helper has one.** The W2+W3 agent is applying it. This matters to W2, W3, W4 and
W6/W7 directly: all four plan behavioural cases with prose names, and W4 §7 and
W2 §9 both say to put them in `tests/operator/test-ledger.cpp` because a
prose-named case in a contract-suite source "would never run". After the
aggregate lands that reasoning changes — such a case runs under the aggregate,
though still without a per-requirement ID. Until it lands, the existing advice
stands.

This is the fourth instance today of one defect; see
[checks that cannot fail](../pitfalls/checks-that-cannot-fail.md), which now
records the family rather than the four instances separately.

### 1.4 Line anchors

| Anchor | W2 | W3 | W4 | W6/W7 | Read 2026-08-10 |
|---|---|---|---|---|---|
| schema fingerprint literal | 372 | 368-373 | 368-373 | 371 | **372** (the `UF_TRY_VALUE` block is 369-374) |
| `expectedTables` | 476-482 | 476-482 | 476-482 | 476-482 | **477-483** |
| `snapshots` DDL | 625-631 | 625-631 | — | — | **626-632** |
| `dispatches` DDL | — | — | 690-699 | — | **691-700** |
| `SessionManifest::operatorProtocolSchemaHash` | 135 | — | — | — | **133** (`policyArtifactHash` is 135) |
| contract-suite include site | — | — | `tests/CMakeLists.txt:352` | — | **`CMakeLists.txt:74-76`**, before `add_subdirectory(tests)` |

Everything else spot-checked matched: the operator schema's `PlanProposal` (483),
`EffectivePlan` (510, twelve members, six echoed and six derived),
`DecisionBasis` (363, four inputs plus the result), `SnapshotParts` (281, fifteen
members), `SnapshotIdentity` (319, those fifteen plus `token_id`, `session_id`,
`snapshot_revision`), `ProjectSnapshot.available_tools` (419) and `event_cursor`
(424); `ProjectVocabulary` at `project-under-test.hpp:27-60`;
`observedReduceInput` at `:89`; `k_liveControllerJoin` at `ledger.cpp:892`;
`sqlite3_busy_timeout(database, 5'000)` at `:402`.

## 2. The rulings, recorded

These are decided. They are restated here with their reasoning so the rest of the
document can be read against them.

**R1 — `s04` is split across two items.** W2 derives `identity_hash` and
`decision_basis_hash` from the parts; W3 derives the parts. Neither half closes
`s04` alone: a derived basis over caller-supplied components still lets a caller
pin a snapshot to a world nobody observed, and composed parts under a
caller-supplied hash still let a caller name the decision. `s04` closes when both
land. `2026-08-10-next-block.md` §2 named one item and now names both.

**R2 — W2 and W3 land as one change.** Both rewrite `createSnapshot` and both
rewrite the `snapshots` table. W3 §8.1.2 already requires the columns to be
unioned and the fingerprint recomputed once. Landed separately they cost two
fingerprint recomputations, two rounds of contract-suite fixture updates, and an
intermediate state that does not build, because W2's `ObservedSnapshotParts`
parameter is deleted by W3 before any caller has been converted to it. W4 is a
separate change after them.

**R3 — `snapshots` keeps the scalar columns the joins need alongside
`canonical_parts`.** W2 wanted the parts as canonical JSON text, which is the
only form the content address can be recomputed from. W3 correctly observed that
SQL cannot join through JSON text, and §3.4's token-staleness join needs
`project_state.revision` and `project_observations.revision` as columns. One fact,
stored once in the form the content address needs and once in the form the query
needs. The DDL in §6 carries that sentence at the table, because the next reader
will ask whether it is two spellings of one thing; it is not, and one test asserts
each scalar equals its member in `canonical_parts`.

**R4 — `p03`'s containment is declaration plus attribution, not prevention.** The
Tool Catalog is project-owned, so a project can mark every tool `Semantic` and
dissolve `p03`'s intent. This is the same containment W2 §1 already accepts for a
plugin that under-declares its own effects: a project describes its own tools,
and `plugin_hash` bound to `project_registration_hash` makes a false description
attributable rather than undetectable. What `p03` enforces is that the Operator
never offers and never accepts a `Privileged` tool for an online Agent — not that
a project cannot mislabel its own catalog. A deployment-principal co-signature
was considered and rejected: it introduces a second trust model beside the one
`ToolMutability` already uses, and two trust models are worse than one documented
limit. The limit is stated at the `ToolSurface` declaration so it is never
mistaken for isolation. §3.12 carries the wording.

**R5 — the schema fingerprint is fragile and every DDL change recomputes it in
the same change.** It is not a named constant: it is a bare string literal in
`verifyExactDatabaseSchema` in `ledger.cpp` (line 372 today) plus the
`expectedTables` list nearby (477-483). It covers the **stored DDL text**, so
reindenting the `R"sql(...)"` block changes it even when the schema is identical.
`initialize()` calls `verifyExactDatabaseSchema` immediately after creating the
schema, so a forgotten recomputation cannot ship green — but a formatting pass
over that block breaks every existing database in a way review will not see. Any
change touching that block recomputes both values in the same change and says so
in the commit message.

**R6 — the `contract-*` / `schema-*` split is per assertion, not per case.**
Project-parameterised store behaviour lives in the exported suite; schema-shape
reads live in the repository. A MIXED case keeping both is honest and is not
split further.

**R7 — W4 is larger than its estimate for a specific reason.**
`contract-control-c03` needs a real `TaskHost`, so the Runtime v2 fixture must be
**extracted** from `tests/task/test-runtime-v2-contract.cpp` into `tests/support/`
rather than copied. Read today, the fixture material in that file spans roughly
lines 47-540 — `TaskHostTestAccess`, `k_authorizeSource`, `FrameSource`,
`ActionSink`, `RuntimeContext`, `loadedRuntime` — about 490 lines. The extraction
is part of W4.

## 3. Conflicts, and how each resolves

A conflict is a place where two documents describe the same type, table, column,
signature or hash differently. Emphasis differences are not listed.

### 3.1 `createSnapshot`'s signature — three shapes

| Document | Signature |
|---|---|
| W2 §7 | `(ControlLease const&, ObservedSnapshotParts const&) -> Result<SnapshotRecord>` |
| W3 §6 | `(ControlLease const&, ProjectPluginHandle const&, task::UiObservationSnapshot const&) -> Result<SnapshotRecord>` |
| W6/W7 §7.1 | `(ControllerBinding const&, MonotonicInstant observedAt, ControlLease const&, ObservedSnapshotParts const&)` |

**W3 is right, and W6/W7 adds two parameters to W3's shape rather than to W2's.**
Under R2 there is no moment at which `ObservedSnapshotParts` exists, so it is
never written. The two intermediate shapes:

```cpp
// after W2+W3 (one change)
auto createSnapshot(
    ControlLease const& lease,
    ProjectPluginHandle const& plugin,
    task::UiObservationSnapshot const& observation
) -> Result<SnapshotRecord>;

// after W6
auto createSnapshot(
    ControllerBinding const& controller,
    MonotonicInstant observedAt,
    ControlLease const& lease,
    ProjectPluginHandle const& plugin,
    task::UiObservationSnapshot const& observation
) -> Result<SnapshotRecord>;
```

Why W3 rather than W2: W2's own §4 says the six members of
`ObservedSnapshotParts` are "the only ones a caller supplies **after W2**", and
W3 removes all six — `observationId`, `stateResolutionHash` and `targetGeneration`
come from the Host-minted `UiObservationSnapshot`, `projectObservationHash` and
`projectObservationRevision` from `derive` inside the transaction, and
`availabilityRevision` from `control_transitions`. A struct whose every member is
deleted by the change landing beside it is a shim, and CLAUDE.md forbids it.

### 3.2 `ObservedSnapshotParts` — created (W2, W6/W7) versus deleted (W3 §8.1.1)

**Never created.** Direct consequence of 3.1. Delete the type from W2 §4's code
block when implementing; the *reasoning* in that block — which members are the
Operator's and must not be accepted from anyone — survives and is what §6's
`snapshots` columns encode.

### 3.3 The `snapshots` table — three column sets

W2 keeps the existing five columns and adds `decision_basis_hash` and
`canonical_parts`. W3 replaces the table with fifteen columns, a
`UNIQUE(session_id, snapshot_revision)` and a composite foreign key, and does
**not** carry `canonical_parts` in its own DDL block even though its §8.1.3 says
it adopts it. W6/W7 adds `event_cursor`.

**Union, per R3. §6 is the single spelling.** W3's DDL block is corrected by W3's
own §8.1.3; that is an internal inconsistency in W3, not a disagreement with W2.

### 3.4 `canonical_parts` versus the scalar columns

W2: parts as canonical JCS text, "both hashes are recomputable from it, which is
what lets §9 falsify the derivation". W3 §3.4: the redeeming join needs
`s.project_state_revision=state.revision` and
`obs.project_state_revision=state.revision`, and SQL cannot join through JSON
text.

**Both. R3.** The scalar columns are exactly the join keys — `plugin_id`,
`project_instance_key`, `project_observation_revision`, `project_state_revision`,
plus the identity fields the record needs on its own. The remaining
`SnapshotParts` members (`controlled_target_id`, `session_manifest_hash`,
`project_state_hash`, `project_observation_hash`, `lease_id`, `fencing_token`)
live only in `canonical_parts` and are reachable through the joins the row
already carries. That is why this is not fifteen columns plus a fifteen-member
blob.

### 3.5 Who derives `decision_basis_hash`

W2 §4 rules "W2 derives `identity_hash` and `decision_basis_hash` from the parts;
W3 derives the parts" and flags it as inferred. W3 §8.1.4 observes that both
documents compute both hashes in the body of `createSnapshot`, so there is no
code conflict — only a wording decision about which item gets credit.

**W3 is right about the code; R1 settles the credit.** There is one derivation,
in `OperatorCoordinator::createSnapshot`. The material is fixed here so it is
written once:

```text
decision_basis_hash = sha256(
  {"project_observation_hash":…,"project_state_hash":…,
   "session_manifest_hash":…,"state_resolution_hash":…}
)
```

— the four `DecisionBasis` inputs as an RFC 8785 JCS object with
`decision_basis_hash` itself omitted, which is what the schema's five-member
definition requires. W2 §4 and W3 §3.3 step 14 already agree on this; it is
recorded because two documents stating the same formula is how a third spelling
gets invented.

```text
identity_hash = sha256(canonical SnapshotParts JCS)
```

— all fifteen members in JCS order (`availability_revision`,
`controlled_target_id`, `decision_basis_hash`, `fencing_token`, `lease_id`,
`observation_id`, `project_instance_key`, `project_observation_hash`,
`project_observation_revision`, `project_state_hash`, `project_state_revision`,
`session_epoch`, `session_manifest_hash`, `state_resolution_hash`,
`target_generation`), which is `canonical_parts` byte for byte.
`snapshots.event_cursor`, `snapshots.token` and `snapshots.snapshot_revision` are
outside both hashes, because they are `SnapshotIdentity` and `ProjectSnapshot`
record data and not composed state — which is what makes two snapshots over an
identical world share an `identity_hash`.

### 3.6 `DispatchReservation` — two shapes

| Document | Shape |
|---|---|
| W2 §7 | `{frozenPlanHash, decisionBasisHash, stepIntentHash, dispatchSequence, operationRevision, stepIndex}` |
| W4 §5.3 | `{task::DispatchAuthority authority; uint64 operationRevision;}` |

Both are right about their own requirement and neither covers the other. W4's
`DispatchAuthority` carries `frozenPlanHash` and `dispatchSequence`; it does not
carry `decisionBasisHash`, `stepIntentHash` or `stepIndex`, which W2 needs the
caller to be able to read and no longer able to invent. The merged shape, landing
with W4:

```cpp
struct DispatchReservation final
{
    task::DispatchAuthority authority;
    ContentHash             decisionBasisHash;
    ContentHash             stepIntentHash;
    uint64                  operationRevision{};
    uint64                  stepIndex{};
};
```

`frozenPlanHash` and `dispatchSequence` are read from `authority` and are not
duplicated, which is W4 §5.3's own rule applied to W2's three fields.

### 3.7 `takeoverLease`'s return type

W4 §5.3 changes it to `Result<ControlTakeover>` so the caller can tell "nothing
was in flight" from "one effect may already have landed". W6/W7 §7.1's table
shows it still returning `Result<ControlLease>`, and W6/W7 §9.2 assumption 7
states "`takeoverLease` keeps its `(session, reason)` shape apart from taking a
binding".

**W4 is right; W6/W7's assumption 7 is falsified.** W4 lands before W6. The final
shape is
`takeoverLease(ControllerBinding const&, std::string const& reason) -> Result<ControlTakeover>`,
and W6/W7's call-site table gains `takeover->lease` everywhere it says `lease`.

### 3.8 The schema fingerprint constant's name

W4 §6 promotes the literal to `k_exactSchemaV1Fingerprint`. W6/W7 §6.3 promotes
it to `k_operatorDatabaseSchemaHash` and adds "the two must not both introduce a
differently named constant". W2 and W3 leave it inline.

**Decided here (not one of R1-R7): the name is `k_exactSchemaV1Fingerprint`, and
the W2+W3 change introduces it.** Rationale: it is the first change to touch the
block, the error message it guards already says "does not match exact v1", and
naming it does not move the fingerprint, which covers stored DDL text and not C++
source. W4 and W6/W7 then only assign to an existing name. `k_operatorDatabaseSchemaHash`
is dropped.

It must **not** be added to `SCHEMA_AUTHORITIES` in
`tests/test-runtime-surface.py`: that gate hashes a *file*, and this is a
fingerprint over `sqlite_schema` rows. W6/W7 §6.3 already says so and it is right.

### 3.9 `expectedTables` — three partial lists and one wrong "complete" one

W2 adds `operation_plans` and `operation_steps`. W3 adds `project_observations`.
W4 adds nothing. W6/W7 §6.3 gives what it calls the full union — and **omits
`project_observations`**, so its list is wrong.

**§6.3 below carries the correct list after each landing.**

### 3.10 `ProjectSnapshot.available_tools` — two owners claimed

W3 §8 assumption 7: "`available_tools` is W2's or W6's, not W3's", and W3 §9.4
says `ProjectSnapshot` cannot be produced at all until `available_tools` and
`event_cursor` have owners. W6/W7 §9.2 assumption 5: "W3 owns
`ProjectSnapshot.available_tools`. W6 supplies the *predicate* and W3 calls it",
flagged there as "the most likely reconciliation conflict".

**W3 is right: W6 owns both.** W3 produces a snapshot *record*, never a
`ProjectSnapshot` wire document, so there is nothing for it to put a tool list
into. W6 owns the predicate (`p03` §3.2) and the offer-side list, and the `s02`
gate must not assert on a `ProjectSnapshot`.

Consequence W3 named and W6/W7 did not: W3 populates `availability_revision` from
`COALESCE(MAX(sequence), 0) FROM control_transitions`, which moves on
acquire/takeover/release. The moment W6 computes an available-tool set,
`availability_revision` must become **that set's** revision, because
`availability_revision` is inside `identity_hash`. That changes what a snapshot
identity means without changing any column, so it is invisible to the compiler
and to the fingerprint. It belongs in W6's commit message and in one test.

### 3.11 `EffectivePlan.risk` as a mutation proxy

W6/W7's original assumption was that the Operator refuses a `read_only` risk for
a `Mutating` descriptor. W2 §1 says the opposite: a plugin under-declaring its
own effects is contained by attribution, not prevention.

**Already reconciled in W6/W7 §9.1 and correct as it stands.** `maximum_mutations`
is kept and consumed, sourced from `ToolMutability` (Tool Catalog) rather than
from `Risk` (plugin-declared). Two differently sourced counts over a partially
overlapping set are two facts. Listed here only so a reader does not re-open it.

### 3.12 `p03`'s containment — R4's wording

W6/W7 open question 1 observes that the Tool Catalog is project-owned, so a
project could mark every tool `Semantic` and dissolve `p03`. **R4 closes it: the
containment is declaration plus attribution, not prevention.**

What `p03` **does** enforce is that the Operator never offers and never accepts a
`Privileged` tool for an online Agent. That is a conjunction evaluated twice, at
offer and at submit, and it is falsifiable (W6/W7 T-P03-a, T-P03-b). The
capability narrows and never widens, which is why `ToolSurface` is a ceiling on
the descriptor rather than an entry in the capability document.

**The limit is stated at the `ToolSurface` declaration**, so it is never mistaken
for isolation:

```cpp
    // Whether a tool's arguments and results are stated in the project's own
    // vocabulary, or in the machine's -- coordinates, pixels, key codes,
    // receipts, fencing tokens, bindings, frames. It is a property of the Tool
    // Catalog descriptor and never of a request.
    //
    // The catalog is project-owned, so this is a declaration the project makes
    // about itself, not isolation the Operator imposes. A project that marks a
    // coordinate tool Semantic is not contained by p03; it is attributable,
    // because the catalog bytes are inside plugin_hash, which is inside
    // project_registration_hash, which pins the session. What p03 enforces is
    // that the Operator never offers or accepts a Privileged tool for an online
    // Agent. It is the same limit W2 accepts for a plugin that under-declares
    // its own effects, and it is deliberate: a second trust model beside
    // ToolMutability's would be worse than one documented limit.
    enum class ToolSurface : uint8
    {
        Semantic,
        Privileged,
    };
```

### 3.13 W2 forbids schema changes; W6/W7 makes three

W2 §2: "**The schema must not change.** Its members come from the frozen v1.9
bundle, and the authority document is explicit: 'Changing any product field,
disposition or ownership still requires a new consumer bundle root.'" Verified at
`2026-08-09-runtime-hardening-rewrite.md:38-40`.

W6/W7 §6.1 changes `schema/umbraflow-operator-v1.schema.json` three ways: adds
`controller_kind` to `OperatorSession`, adds `maximum_no_progress_steps` to
`AgentBudget`, and removes `elapsed_without_progress_ms` from `ProgressMarker`.

**Not covered by R1-R7. Recommendation in §7.1.** This is the largest unresolved
item between the four and it is not a detail: all three are product fields.

### 3.14 Where the fixture's plan documents come from

W2 §7 adds six `ProjectVocabulary` members (`planProposal`,
`mismatchedPlanProposal`, `uiActionIntent`, `waitIntent`, `oversizedPlanProposal`,
`twoStepPlanProposal`). W3 §6.1 adds `observedDeriveInput` to `ProjectUnderTest`
beside `observedReduceInput` (verified at `project-under-test.hpp:89`).

No conflict, but they land in one file in one change under R2, and both fixture
providers (`contract-suite/fixtures/umbraflow/provider.cpp`,
`contract-suite/fixtures/arcana-expedition/provider.cpp`) must satisfy both at
once. Their `ProjectDocumentValidator`s accept only `"{}"` for `Derive`, `Plan`
and `NextStep` on both the input and output branches; all of those must accept
the real documents **instead**, not as well.

## 4. Assumptions one document makes that another does not satisfy

These are the ones that break silently, because nothing in the build catches
them. Numbering follows the holder's own numbering where it has one.

| # | Holder | Assumption | Satisfied? | What breaks, and the action |
|---|---|---|---|---|
| 1 | W3 §8.2 | `reserveDispatch` keeps its `decisionBasisHash`/`frozenPlanHash`/`stepIntentHash` caller parameters until W2 replaces them | **No** — under R2 W2 and W3 land together, so W3 never sees the old signature | None to do, but W3's "if W2 lands first, W3 rebases" contingency is dead text; the merged change writes the new signature once |
| 2 | W3 §8.3 | W2 reads `decision_basis_hash` from the snapshot rather than recomputing it | **Yes**, after §3.5 | `freezePlan` reads `snapshots.decision_basis_hash` through `operations.snapshot_token`. One producer |
| 3 | W3 §8.5 | a snapshot is per step and reusable, so `createSnapshot` stays non-consuming | **Yes** — `contract-state-s03` asserts one token opens several Operations, and W3 preserves it | But see #4: after W6 each `createSnapshot` charges the Agent observation budget, so "cheap enough to run per step" acquires a per-binding ceiling |
| 4 | W6/W7 §5.2 | the observation budget is decremented in `createSnapshot`, "in its own transaction" | **Partly** — W3 makes that transaction also run `plugin.derive` under the write lock | The decrement must be inside the same `BEGIN IMMEDIATE`, before the plugin call, so a refused budget never pays for a derive. State it in W6's commit |
| 5 | W4 §8 A5 | W2's bounded step sequencing keeps "at most one unanswered dispatch per Operation" | **Yes** — W2 §5 refuses `mintNextStep` while a UI-action step has `dispatch_sequence IS NULL`, and `reserveDispatch`'s existing NULL refusal stays | T-9's `resolvedDispatches == 1` stands |
| 6 | W4 §8 A12 | "W4 has no hard code dependency on W3" | **No** | `DispatchAuthority.targetGeneration` has no source before W3: `snapshots.target_generation` is a W3 column, `operations` has none, and `sessions.installed_generation` is the runtime artifact generation, a different quantity. W4 is a hard dependant. See also §7.2 |
| 7 | W4 §8 A9 | five of six control-state operations are serialized by SQLite; re-check against W3 | **Yes** | W3's composition is entirely inside `createSnapshot`'s `BEGIN IMMEDIATE`, so it is a seventh serialized writer, not an unserialized one. What changes is duration: the plugin VM now runs under the write lock, against a 5000 ms `sqlite3_busy_timeout` (`ledger.cpp:402`). Bounded, because the VM is quota-bound |
| 8 | W4 §8 A11 | W3 does not require `OperatorCoordinator` to hold or call a `TaskHost` | **Yes** | W3 takes a `task::UiObservationSnapshot` **value**; the Host mints it and the caller carries it. `operator -> task` is already public and no reverse edge appears. W4 §2's boundary answer holds |
| 9 | W4 §8 A6 | whichever of W2 and W4 lands second recomputes the fingerprint | **Superseded** | Under R2 and R5 there are three landings and each recomputes. §6.3 |
| 10 | W6/W7 §9.2 #7 | `takeoverLease` keeps returning `ControlLease` | **No** — §3.7 | W6/W7's call sites take `takeover->lease` |
| 11 | W6/W7 §9.2 #8 | W4's `recordDeliveryOutcome` appends a `ledger_events` row | **No** — W4 does not know `ledger_events` exists | W7 must add the append when it adds the table, to `recordDeliveryOutcome` as well as to the paths W7 names. Without it T-A01-c passes over an incomplete stream, which is exactly the class of test this repository treats as a defect |
| 12 | W6/W7 §9.1 | "W2 landed while this was being written" | **No** — W2 is a specification; nothing of it is in the tree | The four corrected assumptions in §9.1 are still correct as corrections; the framing is not |
| 13 | W6/W7 T-P02-a | `tests/test-runtime-surface.py` asserts the absence of command columns "by reading `ledger.cpp`" | **No** — that script does not read `ledger.cpp` today | W7 writes a new rule there. So does W3's T8 (`createSnapshot` must not name an `identityHash` parameter). Both are new rules in a gate that has already shipped a check that could not fail — see [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) — so each needs its positive control demonstrated |
| 14 | W2 §8 | `contract-control-c05`, `contract-control-c08`, `contract-state-s04` exist and are registered | **No** — §1.2 | Add new `contract-*` cases; migration report first |
| 15 | W3 §7 | `contract-state-s01`, `contract-state-s02` exist and are registered | **No** — §1.2 | Same |
| 16 | W4 §7 | `contract-control-c03`, `contract-agent-a07` exist | **No** — §1.2 | Same |
| 17 | W6/W7 §8 | `contract-product-p01/p02/p03`, `contract-agent-a01/a02` exist and are "the five IDs `tests/CMakeLists.txt` already requires" | **No** — §1.2 | Same |
| 18 | all four | `tests/operator/project-fixture.hpp` | **No** — §1.1 | Path only |
| 19 | W3 §9.1 | `UiObservationSnapshot::canonicalJcs()` has a producer | **It did not; it does now** — `modules/task/runtime/jcs.luau` landed on 2026-08-10, outside these four items | §4.1. The dependency is closed, but `frameworkBundleHash()` moved with it and the new Luau test was not yet registered in a CTest |

### 4.1 The JCS serializer, which blocked half of W3

W3 §9.1 records the gap: `modules/task/runtime/` held `evidence.luau`,
`explore.luau`, `model.luau`, `observe.luau`, `project.luau` and
`resolution.luau` and no JSON or JCS module — `jsonl.luau` was deleted in the
runtime rewrite — so `UiObservationSnapshot::canonicalJcs()` had no producer and
`TaskHost::observe` could not be implemented live. W3 named two ways forward and
chose neither. Serializing in the Host would mean C++ walking RuntimeModel-derived
structure, which `tests/test-runtime-surface.py` treats as a violation, so it had
to be trusted Luau.

**It landed while this document was being written**, outside these four items:
`modules/task/runtime/jcs.luau` (330 lines, RFC 8785) with `tests/task/test-jcs.luau`
(278 lines). Read on 2026-08-10, it declares itself deliberately distinct from
`model.canonical_bytes` — "different bytes for the same value on purpose and
neither may be substituted for the other" — which is the distinction W3 needs,
since `canonicalJcs()` is the interoperable form and the private length-prefixed
encoding is not.

Three things an implementer must check rather than assume, because the module was
new at the time of reading:

- `cmake/build.cmake:32` globs `*.luau` recursively, so `jcs.luau` enters the
  framework bundle automatically and **`frameworkBundleHash()` has moved**.
  Anything pinning that digest must be re-read.
- Module visibility in this bundle follows sorted file name, so `jcs` is visible
  to `model`, `observe`, `project` and `resolution` and not to `evidence` or
  `explore`. That is the right side of the line for a resolver serializer, but it
  is a property of the name and worth one conscious check.
- `tests/task/test-jcs.luau` was **not** in `TASK_LUAU_TESTS` in
  `tests/CMakeLists.txt` when read, so it ran in no CTest. That may simply be the
  other agent mid-landing; if it persists it is §1.3's family again.

The dependency is therefore satisfied rather than open, and the landing-order
edge in §5 stands as history: it is why W3's Operator half — the value types, the
composition, the DDL, the join, the tests — was designed to be separable and
writable against a fixture-minted `UiObservationSnapshot`.

## 5. Landing order

```
                    [JCS serializer]     landed 2026-08-10, outside these four;
                            │            gated TaskHost::observe only
                            ▼
  W2 + W3  ──────────►  W4  ──────────►  W6  ──────────►  W7
  (one change)           │                                  ▲
                         └──────────────────────────────────┘
                              (ledger_events on delivery)
```

| Edge | Kind | Reason |
|---|---|---|
| W2 → W3 (merged, not sequenced) | **real** | Both rewrite `createSnapshot` and the `snapshots` table. R2. Separately: two fingerprint recomputations, two rounds of exported-fixture updates, and an intermediate state that does not build because W2's `ObservedSnapshotParts` is deleted before any caller adopts it |
| JCS serializer → W3 | **real**, external, **satisfied** | `TaskHost::observe` had no producer for `canonicalJcs()`. `modules/task/runtime/jcs.luau` landed 2026-08-10 (§4.1). It gated only the live Host half; the Operator half was always separable |
| W2+W3 → W4 | **real** | Two distinct dependencies. `reserveDispatch`'s final parameter list is W2's, and W4 changes only its return type — but it changes the same declaration. `DispatchAuthority.targetGeneration` has no source before W3's `snapshots.target_generation` (§4 #6). W4's own A12 denies the second and is wrong |
| W2 → W6 | **real** | `freezePlan` must exist before a `ControllerBinding` can be threaded through it to charge the risk budget, and `Risk` must exist before the risk-unit table |
| W4 → W6 | **real** | `takeoverLease` returns `ControlTakeover` after W4 and takes a `ControllerBinding` after W6. Landing W6 first means writing the signature twice (§3.7) |
| W3 → W6 | **convenience** | W6 adds `event_cursor` to `snapshots`, which W3 rewrites wholesale. Landing W6 first would mean W3 re-deriving a column W6 just added. No correctness edge |
| W6 → W7 | **real** | `ExternalInputFinding.detected_after_cursor` is a `SubscriptionCursor`, so W6 cannot record a finding without the event sequence. W6/W7 already rules that W6 introduces the sequence and W7 adds `subscribe` and the budgets on top |
| W7 → W4's `ledger_events` append | **real, and backwards** | W7 must add the append to `recordDeliveryOutcome`, which W4 wrote. It is W7's work in W4's function (§4 #11) |

W9 (third adversarial review) stays after W2-W4, as `2026-08-10-next-block.md` §3
has it. W11 is independent of all of it and blocks the branch rather than the
design.

## 6. The complete DDL union

This section exists so the schema fingerprint is recomputed once per landing
rather than once per work item, and so no landing has to guess what the final
shape is. Four documents each describe part of one table set; below is all of it.

Everything goes into the one `R"sql(...)"` block in
`OperatorCoordinator::open`'s `initialize`, `ledger.cpp:503-770`. The fingerprint
canonicalizes `sqlite_schema` rows ordered by `(type, name)`, so **declaration
order inside the block does not change the fingerprint** — placement is a
readability choice, and every object is created in one transaction. What *does*
change the fingerprint is the stored text of each object, indentation and
comments included (R5).

### 6.1 New tables

`project_observations` (W3), placed after `project_instances`:

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
                        project_state_revision INTEGER NOT NULL
                            CHECK(project_state_revision >= 0),
                        project_state_hash TEXT NOT NULL,
                        canonical_observation TEXT NOT NULL,
                        observation_hash TEXT NOT NULL,
                        FOREIGN KEY(plugin_id, project_instance_key)
                            REFERENCES project_instances(plugin_id, project_instance_key),
                        PRIMARY KEY(plugin_id, project_instance_key, revision)
                    ) STRICT;
```

`operation_plans` and `operation_steps` (W2), after `operations` and after
`dispatches` respectively, so the composite foreign key target is visible to a
reader:

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

`ledger_events`, `external_input_findings` and `agent_budgets` (W6/W7) are
unchanged from W6/W7 §6.2 and are not restated; nothing in W2, W3 or W4 touches
them.

### 6.2 Changed tables

`snapshots` — the union. **W2 contributes `decision_basis_hash` and
`canonical_parts`; W3 contributes everything else new; W7 contributes
`event_cursor`.**

```sql
                    -- canonical_parts is the exact SnapshotParts JCS and is the
                    -- only thing identity_hash and decision_basis_hash are
                    -- recomputable from, which is what lets a test falsify the
                    -- derivation. The scalar columns below it are not a second
                    -- spelling of the same fact: they are the join keys, and SQL
                    -- cannot join through JSON text. createOrLoadOperation
                    -- compares project_state_revision and
                    -- project_observation_revision against the live rows, so a
                    -- token goes stale when the composed world moves and not
                    -- only when the lease does. One test asserts each scalar
                    -- equals its member in canonical_parts.
                    --
                    -- token, snapshot_revision and event_cursor are deliberately
                    -- outside canonical_parts: they are record naming and the
                    -- subscription join point, not composed state, so two
                    -- snapshots over an identical world share an identity_hash.
                    CREATE TABLE IF NOT EXISTS snapshots(
                        token TEXT PRIMARY KEY,
                        session_id TEXT NOT NULL REFERENCES sessions(session_id),
                        snapshot_revision INTEGER NOT NULL
                            CHECK(snapshot_revision > 0),
                        session_epoch INTEGER NOT NULL CHECK(session_epoch > 0),
                        identity_hash TEXT NOT NULL,
                        decision_basis_hash TEXT NOT NULL,
                        canonical_parts TEXT NOT NULL,
                        lease_revision INTEGER NOT NULL CHECK(lease_revision > 0),
                        plugin_id TEXT NOT NULL,
                        project_instance_key TEXT NOT NULL,
                        observation_id TEXT NOT NULL,
                        target_generation INTEGER NOT NULL
                            CHECK(target_generation > 0),
                        state_resolution_hash TEXT NOT NULL,
                        project_observation_revision INTEGER NOT NULL
                            CHECK(project_observation_revision > 0),
                        project_state_revision INTEGER NOT NULL
                            CHECK(project_state_revision >= 0),
                        availability_revision INTEGER NOT NULL
                            CHECK(availability_revision >= 0),
                        event_cursor INTEGER NOT NULL CHECK(event_cursor >= 0),
                        UNIQUE(session_id, snapshot_revision),
                        FOREIGN KEY(plugin_id, project_instance_key,
                                    project_observation_revision)
                            REFERENCES project_observations(
                                plugin_id, project_instance_key, revision
                            )
                    ) STRICT;
```

`dispatches` (W4) — one changed column and one new one:

```sql
                        delivery_outcome TEXT
                            CHECK(delivery_outcome IN (
                                'not_delivered', 'delivered', 'transport_unknown'
                            )),
                        delivery_reason TEXT,
                        CHECK(
                            (delivery_outcome IS NULL AND delivery_reason IS NULL)
                            OR (delivery_outcome = 'delivered'
                                AND delivery_reason IS NULL)
                            OR (delivery_outcome IN ('not_delivered',
                                                     'transport_unknown')
                                AND delivery_reason IS NOT NULL)
                        ),
```

`sessions` (W6) — one new column:

```sql
                        controller_kind TEXT NOT NULL
                            CHECK(controller_kind IN ('script', 'agent', 'human')),
```

### 6.3 `expectedTables` and the fingerprint, per landing

Three landings, three recomputations. Each recomputes **both** values in the same
change and says so in the commit message (R5).

| After | Table count | `expectedTables`, sorted |
|---|---|---|
| today | 18 | `approvals,authority_decisions,control_leases,control_transitions,dispatches,fencing_high_water,journal_events,operations,project_instances,project_registrations,project_state,reconciliations,runtime_artifacts,runtime_installations,runtime_publications,runtime_state,sessions,snapshots` |
| W2+W3 | 21 | adds `operation_plans`, `operation_steps`, `project_observations` |
| W4 | 21 | unchanged — no new table |
| W6+W7 | 24 | adds `agent_budgets`, `external_input_findings`, `ledger_events` |

Final list after W6+W7, which is what W6/W7 §6.3 meant to write:

```text
agent_budgets,approvals,authority_decisions,control_leases,control_transitions,
dispatches,external_input_findings,fencing_high_water,journal_events,
ledger_events,operation_plans,operation_steps,operations,project_instances,
project_observations,project_registrations,project_state,reconciliations,
runtime_artifacts,runtime_installations,runtime_publications,runtime_state,
sessions,snapshots
```

Sorting is SQLite `BINARY`, so `operation_plans` and `operation_steps` precede
`operations` (`_` is 0x5F, `s` is 0x73).

`PRAGMA user_version` stays `1` through all three. All four documents already
agree, for the same reason: a second accepted value would be a second spelling of
"the schema" with no reader.

**Recompute from a freshly created database, never by hand.** Either a throwaway
script outside the tree, or temporary `[DEBUG-…]` instrumentation in
`verifyExactDatabaseSchema` that prints `actual.hex()`, removed before the change
is complete:

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

Each landing's commit message states: any existing `operator-runtime.sqlite`
stops opening, with "Operator database table set does not match schema v1"
first — the table check at `ledger.cpp:484-490` runs before the fingerprint — and
the migration is to delete the file. There is no reader for the old shape and
none may be added.

## 7. Conflicts the rulings do not cover

### 7.1 W6/W7's three schema changes (from §3.13)

W2 §2 forbids changing `schema/umbraflow-operator-v1.schema.json`; W6/W7 §6.1
changes three definitions in it. The authority is explicit that a product-field
change requires a new consumer bundle root, and migration-report stop condition 2
requires the report updated first. All three of W6/W7's changes are product-field
changes.

**Recommendation, in decreasing confidence:**

- **`ProgressMarker.elapsed_without_progress_ms` — do not remove it.** W6/W7 §5.3
  removes it because a second millisecond ceiling beside `maximum_elapsed_ms`
  would be two spellings of one axis. That reasoning is about the *budget*, not
  about the *marker*: the marker is a report of what happened, and reporting
  elapsed-without-progress alongside `same_state_repetitions` is one fact each,
  not two spellings. Leave the field in the schema, leave it unconsumed, and let
  W6/W7's own open question 4 stay open. Removing a required member of a frozen
  definition to avoid an unconsumed proof field is the more expensive of the two
  mistakes.
- **`AgentBudget.maximum_no_progress_steps` — do not add it.** The five axes
  `a02` names are action, risk, time, observation and no-progress; the schema's
  `AgentBudget` already carries `maximum_tool_calls`, `maximum_mutations`,
  `maximum_observations`, `maximum_elapsed_ms` and `maximum_risk_units`. Make the
  no-progress ceiling **Operator-owned**, a `constexpr` beside the risk-unit
  table, the way W2 makes `k_workflowCeiling` Operator-owned rather than a
  proposal field. That closes `a02` without touching the bundle.
- **`OperatorSession.controller_kind` — needs an upstream decision.** It is
  genuinely new product state and the ledger cannot express `p01` without it. The
  `sessions.controller_kind` column can land without the JSON change, because
  nothing serializes an `OperatorSession` document today. Do that, and raise the
  schema addition as a bundle-root question rather than smuggling it in under a
  work item. If the answer is "add it", the migration report is updated first and
  the commit says the bundle root moved.

If any of the three does land, `SessionManifest`-derived
`operator_protocol_schema_hash` values change for every real deployment, which is
correct and intended, and `session_manifest_hash` moves with them — which moves
every `decision_basis_hash`, because `session_manifest_hash` is one of its four
inputs. That is a wider blast radius than "a JSON edit" suggests and belongs in
whichever commit makes it.

### 7.2 `DispatchAuthority.targetGeneration` means two different things

W4 §5.1 declares `uint64 targetGeneration{}` on `DispatchAuthority` and §4.2 says
the Host refuses an authority "naming another target, generation or epoch". But
`TaskHost::PendingReceipt` carries a `GenerationId` — the runtime-model
generation — and `TaskHost` has no notion of `TargetGeneration`, which is a
`domain` quantity carried by `Frame`, `FrameIdentity`, `Detection` and
`ObservationLease`. The schema's `SnapshotParts.target_generation` is the second
one.

So W4's field is either (a) the runtime `GenerationId`, in which case the Host
can check it, the ledger sources it from `sessions.installed_generation`, and W4
has no W3 dependency for it; or (b) the target's `TargetGeneration`, in which
case only W3's `snapshots.target_generation` supplies it and the Host **cannot**
check it, so it is carried through untouched like `operationId`.

**Recommendation: both, under two names.** `runtimeGeneration` (a `GenerationId`)
is what the Host checks and is what W4 §4.2's refusal is really about;
`targetGeneration` (a `TargetGeneration`) is what W3 records and what the
delivery-time freshness comparison W3 §3.5 defers to W4 actually needs. One field
carrying two quantities is exactly the drift these documents keep catching
elsewhere. Whichever is chosen, W4's A12 is wrong either way if `targetGeneration`
survives (§4 #6).

### 7.3 `AgentBudget`'s C++ names do not match the schema's

W6/W7 §5.1's `AgentBudget` has `maximumActions`; the schema's `AgentBudget` has
`maximum_tool_calls`. Verified in `schema/umbraflow-operator-v1.schema.json`.
`maximumRiskUnits`/`maximum_risk_units`, `maximumObservations`,
`maximumElapsedMillis` and `maximumMutations` all correspond; only the first does
not. Two spellings of one thing.

**Recommendation: the C++ member is `maximumToolCalls`.** The schema is the frozen
side. W6/W7's §5.2 table calls the axis "action" in prose, which is fine — prose
naming an axis is not a second field.

### 7.4 The naming of the fingerprint constant

Decided in §3.8 rather than left open, because two branches introducing two names
for one literal is a merge conflict on the thing R5 exists to protect.

## 8. What could not be verified

- **The 19 `schema-*` gates were never individually falsified.** Nobody has
  confirmed that each turns red when the schema definition it reads is removed.
  They almost certainly would — reading that definition is all they do — but
  "almost certainly" is what this repository's own falsification discipline
  refuses to accept elsewhere. Recorded as unverified rather than assumed.
- **The gate list will move again.** It was re-read after W10's rename landed and
  matches the counts in §1.2, but three of the four work items still owe it new
  `contract-*` IDs. No work item should copy an ID list out of this document; re-read
  `tests/CMakeLists.txt` and both fixture `CMakeLists.txt` files immediately before
  implementing.
- **`a03` and `a05`.** The requirement is closed by `tools/annotate` and gated by
  the aggregate CTest `test-annotate-backend`, 43 tests under one name. There is
  still **no per-requirement CTest ID for the behavioural half**. The C++ cases
  are now `schema-agent-a03` / `schema-agent-a05`, and — read today — they are no
  longer pure schema-shape reads: behavioural assertions against
  `prepared.project.journalSchemaOwner` have been appended to at least `a03`. A
  case that exercises code while wearing the `schema-` prefix contradicts the
  vocabulary comment in `tests/CMakeLists.txt:43-56`, which says `schema-` "passes
  whether or not the behaviour exists". Either those assertions belong in a
  `contract-agent-a03` case or the prefix is wrong. Not W2-W7's to settle;
  recorded because it is the one place where the renamed vocabulary currently
  overstates.
- **`k_workflowCeiling`'s values** (W2 open question 2). 64/64/256/64/600000 are
  invented in W2. Nothing in the frozen bundle, the schema or the migration
  report names a ceiling, and a search of the tree confirms it. Unresolved, and
  it is a number an implementer will otherwise silently keep.
- **`required_approvals` as 0/1** (W2 open question 3). The schema types it as an
  array of identifiers. W2 collapses it because the ledger's only approval kind is
  the single `approvals` row. If the array is meant to name distinct approver
  capabilities, both the column and the `approvals` table need a different shape.
  Unresolved; larger than W2.
- **`controlled_target_id` versus `controlled_target_key`** (W3 open question 2).
  The schema member is `controlled_target_id`; every table and struct in the code
  spells it `controlled_target_key`. Two spellings of one thing, predating all
  four items. Whichever survives must be applied to the schema and the DDL in one
  change. Not settled here, and not W2-W7's to settle alone.
- **W4's `Host::deliver` line anchors** (`task-host.hpp:126-130, 190, 205, 248,
  257, 263`) were checked and match. Its `ledger.cpp` anchors were spot-checked
  and are within a line or two. The `contract-suite/fixtures/arcana-expedition/provider.cpp`
  anchors in W2 and W3 were not individually checked.
- **Whether any of the fifteen W2, eight W3, sixteen W4 or twenty-three W6/W7
  falsifying mutations actually turn their case red.** None has been run. Each
  document is explicit that a mutation leaving a case green means the case is the
  defect; that discipline is not discharged by any of them and is not discharged
  here.
