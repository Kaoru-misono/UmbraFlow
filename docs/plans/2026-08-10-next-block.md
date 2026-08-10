# The next block: complete coverage of the remaining design

Status: **the requirement block is closed.** Every work item that closes a
requirement — W1 through W8, with W10 folded into them — has landed, and all 42
`REQUIRED_CORE` requirements are implemented. W1, W5, W8 and the contract suite
raised in §5 landed on 2026-08-10; W3 (`7cef402`, `4b955de`), W2 (`848e390`), W4
(`e64c143`, `25f57f9`), W6 (`93698b4`) and W7 (`c23efd3`) landed on 2026-08-11.
W0 ran on 2026-08-10 and returned half a pass, which opened W11.

**What remains is not requirement coverage.** W9 (the adversarial round over
W2-W7), W11 (`clang-analysis`, which blocks the branch), W12's retroactive
import review, per-requirement IDs for `a03`/`a05`, and the unresolved defects
and open questions §2 and §6 name. The consumer-side work is a different stage
and is specified in
[consumer attestation](2026-08-11-consumer-attestation.md).
Date: 2026-08-10 (revised the same day, against the landed tree; requirement
state brought current 2026-08-11, and again after the last four landings)
Scope: `umbraflow-cpp` only. No consumer-project writes.
Bundle: v1.9, root `c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`

This plan is derived from the requirement matrix, not from what happened to be
found. Every `REQUIRED_CORE` requirement appears in §2 exactly once, with the
work item — or items — that close it. Nothing in the design is left unassigned.

That invariant was re-checked after the last four landings: **40 in the Done
list plus 2 in the table, no ID in both, 42 in total.** The two are exactly
`UF_SCHEMA_ONLY_REQUIREMENTS` in `tests/CMakeLists.txt`, which CMake derives
from the registered gates and refuses to disagree with — so the table below is
checkable against a configure run rather than by reading.

> Two earlier readings, kept because each was true when made and neither is
> wrong: 28 in the Done list plus 14 in the table on 2026-08-10, and 33 plus 9
> on 2026-08-11 after W3 and W2. `s04` was the one row naming two items, W2 +
> W3, because neither half closed it alone; both landed and it left the table.

W2, W3, W4 and W6/W7 were each written out in full on 2026-08-10 by agents who
could not see each other's drafts. Their conflicts, their unsatisfied
cross-assumptions, their landing order and the union of their DDL changes are
reconciled in
[**W2-W7 reconciliation**](2026-08-10-w2-w7-reconciliation.md), which governed
all four. All four have landed, so it and they are now records; each of the four
carries a dated note saying what the implementation refused and why.

## 1. What the matrix says we owe

51 requirements. `D-01`-`D-09` are the Chaos content pipeline, owned by the
consumer repository and correctly ungated here. The other **42 are
`REQUIRED_CORE` and every one is ours.**

> **Corrected 2026-08-11: two markings, not one, and the `PROJECT_CONTRACT` set
> is not the `D-*` block.** This paragraph said "`D-01`-`D-09` are
> `PROJECT_CONTRACT`". The deciding artifact is the v1.9 requirements matrix,
> `requirements-traceability.md` §4, under the bundle root above: `D-01`-`D-08`
> carry `PROJECT_CONTRACT` and **`D-09` carries `PHASED`**, the only row in the
> matrix that does. `C-11` and `A-04` carry `REQUIRED_CORE` **and**
> `PROJECT_CONTRACT`, so the consumer owes a half of each and its obligation
> list is eleven requirements rather than nine. The 42 / 51 arithmetic below is
> unaffected: 42 `REQUIRED_CORE` including those two, 8 `PROJECT_CONTRACT`-only,
> 1 `PHASED`. What the marking changes is what a consumer attests — a `PHASED`
> requirement attests a declared scope boundary, a `PROJECT_CONTRACT`
> requirement attests a property — which is specified in
> [consumer attestation](2026-08-11-consumer-attestation.md) §3 and §8.

All 42 have at least one registered CTest gate. But a gate is not a proof, so
since W10's rename landed on 2026-08-10 the gates carry two prefixes and say
which kind they are:

- `contract-<area>-<id>`, label `CI;CONTRACT` — exercises the code and goes red
  when the behaviour is removed. **40 of them** now that the block is closed; 33
  after W3 and W2, and 28 when this section was written on 2026-08-10.
- `schema-<area>-<id>`, label `CI;SCHEMA` — reads a `schema/*.json` file and
  asserts a definition exists with certain members. It passes whether or not the
  behaviour exists, which is why it may not wear the `contract-` name. **19 of
  them**, unchanged throughout.

**59 gates over 42 requirements, and that is correct rather than an arithmetic
slip.** One requirement may own one gate of each kind, guarding different things.
`c09` through `c13` do: the store behaviour is a `contract-control-cNN` in the
exported suite, and the schema symbol the migration report defines that ID by is
a `schema-control-cNN` in `tests/operator/test-control-contract.cpp`. Twelve
more joined them on 2026-08-11 by the same rule — `s01 s02 s04 c05 c08` with W3
and W2, `c03 a07` with W4, `p01 p02 p03` with W6, `a01 a02` with W7 — the
behaviour that closed each one being a new `contract-` case while the
schema-shape read it already had keeps its `schema-` name rather than being
folded in. What the vocabulary forbids is a name that overstates — a `contract-`
gate whose assertions only read a schema file. Anyone who reads "59 and 42" as a
mistake and tries to reconcile the two numbers will delete a real gate.

The whole run is 83 registered tests, of which 40 carry `CONTRACT` and 19 carry
`SCHEMA`; the rest are the four `CONTRACT-SUITE` aggregates, the module
regression binaries, the four Luau contracts and the Python gates.

The 40 requirements owning a `contract-*` gate are exactly the Done list in §2.
Fourteen IDs were renamed to `schema-*`: `p01 p02 p03`, `s01 s02 s04`,
`c03 c05 c08`, `a01 a02 a07` — the twelve this plan is for — plus `a03` and `a05`,
which are the different case described below. Five `schema-control-c09` through
`c13` were added new, for the schema halves of requirements whose behaviour moved
to the exported suite.

The rename made the matrix honest; it did not close anything. Closing the twelve
is what §2 and §3 are for, and each of those work items now owes a **new**
`contract-<area>-<id>` case — the migration report updated first, then
`tests/CMakeLists.txt`, then a suite's `CASES` list — rather than a rewrite of a
case that no longer exists under that name.

All twelve were closed that way on 2026-08-11 — `s01 s02 s04 c05 c08` by W3 and
W2, `c03 a07` by W4, `p01 p02 p03` by W6, `a01 a02` by W7 — and **the order was
inverted every single time.** Each landing registered its cases and left the
migration report naming a `schema-` spelling; the report was repaired after each
rather than the rule relaxed. Six commits now, counting `dcc43b5` and R3-F3,
with no counter-example. The report is the one document in this chain that no
gate reads, which is why it is always the side that drifts; that conclusion is
recorded in the report itself rather than only here, because the next agent will
read the report and not this paragraph.

The 19 `schema-*` gates **were** falsified on 2026-08-10, by
[the third adversarial round](../reviews/2026-08-10-third-round-review.md).
Each of the 38 definition names they pass to `definition(schema, name)` occurs
exactly once in its schema file, so the lookup cannot latch onto a `$ref` or a
`required` entry and removing a definition fires
`REQUIRE(namePosition != std::string::npos)`. One weakness survives, finer
grained than the gate: the field checks are substring searches over the whole
definition text, so a property moved out of `properties` but left in `required`
is still found.

`a03` and `a05` are a different case again since W5, and the difference must be
stated precisely because the arithmetic invites the wrong reading. **Both
requirements are implemented and both are gated.** The behaviour landed in
`25520a3` on the Python side and runs on every CI run under the aggregate CTest
`test-annotate-backend`. What is missing is a **per-requirement CTest ID for the
behavioural half** — a name, not a gate and not the code. W10's rename did not
create one; it moved the C++ cases to `schema-agent-a03` and `schema-agent-a05`.
So "40 of 42 have a behavioural gate" is a statement about the naming scheme,
and reading it as "two requirements are unimplemented" is wrong in both halves.

> **Corrected 2026-08-11:** this paragraph also said `schema-agent-a03` had
> gained behavioural assertions during the rename and so proved more than its
> prefix claims. That is no longer true of the tree — those assertions sit in
> `contract-agent-a04`, and `schema-agent-a03` reads four schema files and
> asserts definition shape and nothing else. The vocabulary does not overstate
> or understate anywhere.

## 2. Every requirement, and what closes it

**Done — behavioural gate exists (40).** `p04 p05 p06`, `u01`-`u08`,
`s05`, `c01 c06 c07 c09 c10 c11 c12 c13 c14`, `a08`; since W1 landed on
2026-08-10, `c02 c04 s03 s06 a04 a06`; and on 2026-08-11, `s01 s02`
(`4b955de`), `s04 c05 c08` (`848e390`), `c03 a07` (`25f57f9`), `p01 p02 p03`
(`93698b4`) and `a01 a02` (`c23efd3`). No further work; they are re-verified by
the existing suite on every run.

One limit inside that six is worth carrying rather than smoothing over. Two of
`s06`'s assertions cannot be falsified on their own: the refusal of a second
baseline and the refusal of a session pinned to an unprovisioned key are
already guaranteed by `PRIMARY KEY(plugin_id, project_instance_key)` on
`project_instances` and by the sessions foreign key into it, so no code change
can turn those two red while the schema stands. The falsifiable core of the
case is the ABA commit itself, where a stale
`expectedProjectStateRevision` of 0 is refused because another instance really
does sit at revision 0.

**The five that closed on 2026-08-11 carry the same kind of limit, and it is
larger.** All 23 of W2's and W3's falsifying mutations were run — the first time
any of this block's were — and three of W2's stay green. Each is a defect in the
test, not in the code, and none is closed by the requirement being marked done.
W3's run found less but not nothing; its three retargeted or contradicted rows
are recorded in [its own specification](2026-08-10-w3-snapshot-coordinator.md).

- The registration guard in `OperatorPlanAuthority::mintPlan` that W2 §9 T2
  names is not the guard doing the work. `freezePlan` has already checked the
  plugin against the session, so it refuses first and the case passes with the
  named guard disabled. The guard stays as defence in depth; what does not
  stand is the claim that T2 proves it.
- "A plan freezes once" (T10) is enforced by the state machine refusing a
  non-`Proposed` Operation, not by `operation_plans`' primary key. The key is a
  real second enforcement, and W2's own argument against double enforcement
  applies to it.
- **The audit row has no reader** (T13). Corrupting
  `authority_decisions.decision_basis_hash` is invisible to every test, because
  `reserveDispatch` reads the basis from `operation_plans` and nothing on the
  public surface reads `authority_decisions` back. Recorded as an instance of
  the repository's own recurring family in
  [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md), which is
  where it belongs: it is not a fact about `c05`, it is a fact about what an
  audit column is worth without a reader.

Three things W2 specified are stored and unenforced, and closing `c08` did not
close them. `maximum_observations`, `maximum_waits` and `maximum_elapsed_ms` are
clamped against `k_workflowCeiling` at mint and written into `operation_plans`,
and no count is ever compared against them — only `maximum_steps` and
`maximum_dispatches` refuse. `StepKind::Wait` is never exercised, because
neither fixture answers `next_step` with a `WaitIntent`, so the wait branch of
`readStepIntent` and the `kind() == StepKind::UiAction` guard in the ledger are
untested on that side; W2 §11 questions 4 and 5 are the open design half of
that. And `ApprovalRequest::policyHash` remains a caller field — `c12`'s
recorded debt, with `c12` counted done.

**The last four landings carry more limits of the same kind, and they are this
block's most valuable output.** Every mutation W4, W6 and W7 specified was
run — 18 in `e64c143` and 19 in `25f57f9`, and full campaigns for W6 and W7,
W6's rerun from scratch after its harness was found defective. The red ones say
only that the specification was followed, so the green ones are the report.
Each limit below is a property that is true, is
load-bearing, and that **no mutation of this tree can turn red** — recorded here
beside the requirement it qualifies, because a requirement marked done with an
unfalsifiable half is exactly what a later reader will over-read. The repeatable
shapes are in [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md);
what follows is which requirement each one qualifies.

- **`c03`, `a07` — the lease identity is one fact spelled three times.**
  `lease_id`, `fencing_token` and `lease.revision` always move together: the
  revision is set equal to the fencing token, and both move with the lease id on
  every acquire, release and takeover. Only the conjunction is falsifiable. A
  case was written specifically to separate them, using the one schedule where
  the audit row and the live lease disagree, and even that cannot.
- **`a07` — "a takeover resolves only its own target's dispatches" has no test
  at all.** Its mutation goes red for the wrong reason, a bind-parameter range
  error, and every fixture has exactly one controlled target, so the property is
  unobserved rather than merely weakly held. Closing it needs a second
  controlled target in a fixture, which is a fixture change and not a code
  change.
- **`c03` — `dispatches.delivery_reason` is written, constrained, and read back
  by nothing.** A three-way `CHECK` ties its nullness to `delivery_outcome`, so
  the rule that only `not_delivered` proves absence is guarded; the *text* is
  not. Same shape as `authority_decisions.decision_basis_hash` above, one
  requirement over.
- **`u06` — `HostDeliveryReport`'s friend count is inexpressible.** That the
  constructor has exactly one friend is the whole mechanism stopping a test
  harness from fabricating a delivery that never ran, and no test can state it:
  adding a second friend compiles and every case stays green. Confirmed
  empirically rather than assumed, which is the only honest form of that claim.
- **`p01` — `requireLiveBinding` was four conjuncts that each masked the
  others.** Every field it compared had been copied out of the very row it
  compared against, the row is immutable, and only `bindController` can mint a
  binding. The epoch conjunct was a second recording of the same fact as the
  active flag, because opening the database begins a new epoch and deactivates
  the previous one's sessions. Unlike the others this one was **repaired**:
  two conjuncts now, one of which a single gate falsifies.
- **`a02` — the budget-presence invariant cannot be turned red.** "A budget row
  is present exactly when the controller kind requires one" has both sides
  written by the same authority, so any mutation moves them in step. It is kept
  because it converts a silent no-charge into a loud failure, and it is named as
  unfalsifiable at the site rather than counted as coverage.
- **`p01` — two more, carried without repair.** `OperationMachine` takes no
  controller kind and cannot, so `p01`'s state-machine claim has no kind-varying
  mutation; that impossibility *is* the property. And the partial index on one
  active mutation per target is a second spelling of the C++ check with a wider
  scope.

One zero from this set was **closed within the block**, and the contrast is the
point: `ledger_events`' descriptive columns had no reader when W6 wrote them,
which W6 recorded as a debt with `subscribe` named as the reader, and W7 became
that reader one landing later. A write-only column is a debt when the commit
adding it says so and names what will read it, and a defect otherwise.

**The 2 that own a `schema-*` gate and no `contract-*` gate.** This set is
`UF_SCHEMA_ONLY_REQUIREMENTS` in `tests/CMakeLists.txt`; CMake derives the same
set from the registered gates and fails configure if the two disagree, so a row
leaves this table only when a `contract-` case for it exists. **Neither row is
open work** — both requirements are implemented and gated; what each lacks is a
per-requirement ID:

| ID | Requirement | Implementation state | What is missing |
|---|---|---|---|
| `a03` | Audit Trace, Ledger, Journal and Replay Bundle are separate | **implemented** in `25520a3` (W5); the bundle closure runs on every CI run under the aggregate `test-annotate-backend`; the C++ case is `schema-agent-a03` | a per-requirement CTest ID for the behavioural half |
| `a05` | UI replay and project/operation replay are independent gates | **implemented** in `25520a3` (W5); both sub-gates are `required` and neither is satisfied by the other's evidence; same aggregate; the C++ case is `schema-agent-a05` | a per-requirement CTest ID for the behavioural half |

Closing these two is naming work, not implementation work: a
`contract-agent-a03` and `contract-agent-a05` that reach the Python behaviour
from CTest, the migration report updated first, then `tests/CMakeLists.txt`,
then `UF_SCHEMA_ONLY_REQUIREMENTS` emptied. It is the only place in the matrix
where a gate name is less than what the tree does.

## 3. The work items, ordered by dependency

```
W0 merge readiness ─── independent                             ran
W1 coverage debt ───── independent                             landed
W3 then W2 ─────────── EffectivePlan and Snapshot Coordinator  landed
      │                (ruled one change; landed as two, W3 first)
      ├── W4 delivery join ── W6 controller facade ── W7 Agent surface   all landed
W5 replay gates ───── independent of W2-W4                     landed
W8 artifact GC ─────── independent                             landed
W9 review round ────── after W2-W7                             OPEN
W11 clang-tidy ─────── independent, opened by W0               OPEN
W12 core admission ─── independent, predates this block        half open
```

The chain held: W4 needed W2 and W3 for the plan and the snapshot, W6 needed W4
because W4 changed `takeoverLease`'s return type, and W7 needed W6's event
sequence to have a cursor to subscribe from. Each landed in that order and none
had to be re-opened.

**W2 and W3 were ruled one change** ([reconciliation](2026-08-10-w2-w7-reconciliation.md)
R2) and landed as two, W3 first (`4b955de`) and W2 second (`848e390`). The
hazard R2 named did not occur, and the reason is that the order was reversed
rather than the ruling ignored: W3 gave `createSnapshot` its final shape —
`(lease, plugin, observation)`, composing rather than accepting — so W2 never
introduced the `ObservedSnapshotParts` parameter that W3 would have had to
delete. The cost R2 predicted was paid: two fingerprint recomputations rather
than one. W4 is a separate change after them, and W6 needs W4 as well as W2
because W4 changes `takeoverLease`'s return type.

| # | Item | Closes | Depends on | Cost | Status |
|---|---|---|---|---|---|
| W0 | Merge readiness: run `linux-analysis` and the three sanitizer presets locally. CI runs on `master` only, so the first CI sight of this work is post-merge; seven of eight configurations are reproducible locally, macOS is not | — | none | 1 day | **ran 2026-08-10**: sanitizers pass, `linux-analysis` fails; the failure is W11 |
| W1 | **Coverage debt**: write behavioural cases for the six requirements whose implementation already exists but whose gate only reads a schema | `c02 c04 s03 s06 a04 a06` | none | 2-3 days | **done 2026-08-10** |
| W2 | **EffectivePlan authority**: mint it from a plugin `PlanProposal` bound to registration, command fingerprint and decision basis; derive the frozen plan, step intent and effect envelope hashes from it; add bounded step sequencing; `reserveDispatch` takes the minted plan instead of three caller hashes | `c05 c08` + half of `s04` | none | 5-7 days | **done 2026-08-11** (`848e390`), with three deviations and three unresolved test defects recorded in §2 and in [the specification](2026-08-10-w2-effective-plan.md) |
| W3 | **Snapshot Coordinator**: introduce `ProjectObservation`; compose UI observation, `plugin.derive` and current ProjectState atomically; derive the snapshot parts instead of accepting them | `s01 s02` + half of `s04` | the JCS serializer, which landed 2026-08-10 | 4 days | **done**: additive half 2026-08-10 (`7cef402`: `TaskHost::observe` and `UiObservationSnapshot`, no signature changed), Operator half `4b955de` |
| W4 | **Join Host delivery to the ledger**: `recordDeliveryOutcome` takes what `Host::deliver` returned, inside the fence; the takeover path enters the same linearization | `c03 a07` | W2+W3, both landed | 3 days **understated**: `contract-control-c03` needs a real `TaskHost`, so the ~490-line Runtime v2 fixture in `tests/task/test-runtime-v2-contract.cpp` must be extracted and shared, not copied | **done 2026-08-11** in two commits — `e64c143` (a `HostDeliveryReport` only `TaskHost` can mint) and `25f57f9` (the ledger join). The extraction was done first, in `55bd564`. Deviations and four unfalsifiable properties in [the specification](2026-08-10-w4-delivery-join.md) and §2 |
| W5 | **Replay Bundle and the two gates**: implement the bundle closure and both publication gates rather than declaring them | `a03 a05` | none | 4 days | **done 2026-08-10** |
| W6 | **Controller facade**: one path for Script, Agent and Human; out-of-band human input enters as an external source; the Agent surface is semantic-only | `p01 p02 p03` | W2+W3, **and W4** | 4 days | **done 2026-08-11** (`93698b4`). `p02` closed by making a command inexpressible rather than by refusing one; `ExternalInputSource` was refused. See [the specification](2026-08-10-w6-w7-controller-and-agent.md) |
| W7 | **Agent subscription and budgets**: `subscribe(after_cursor)`, and action/risk/time/observation/no-progress budgets | `a01 a02` | W6 | 4 days | **done 2026-08-11** (`c23efd3`). Budgets are established at `pinSession`, never at a door the controller comes through; six specification clauses refused |
| W8 | Artifact GC by database refcount for orphaned `runtime-artifacts/<hash>/` and `.staging` | — | none | 1-2 days | **done 2026-08-10** |
| W9 | Adversarial review round over W2-W7 | — | W2-W7, all landed | 1 day per reviewer | **open, and now unblocked.** The round that ran on 2026-08-10 covered `37296d7..cec8898`, which ends before W2; nothing has reviewed W2, W3, W4, W6 or W7 |
| W10 | Rename the schema-shape gates `schema-*` as each requirement gains a behavioural gate, so the matrix never overstates | — | tracks W1-W7 | folded in | **done.** The twelve open IDs were renamed 2026-08-10 and all twelve gained a `contract-` case on 2026-08-11. Nothing is left to track: no gate name now overstates or understates |
| W11 | **Make the clang-tidy analysis presets compile**: the `*-analysis` presets do not build, under the project's own `-Wunsafe-buffer-usage` and under `WarningsAsErrors: '*'`. Its scope is whatever those presets still refuse, never a count — the count moved three times on the day it was written. Opened 2026-08-10 by W0's result | — | none | unestimated; not one day | open and shrinking. Done when a `linux-analysis` build reports nothing **and states how many objects it analysed** |
| W12 | **Core admission debt**: give every `core` facility with no caller outside its own capability test a recorded `evaluate-core-capability` answer, and review the 2026-07-20 template import that admitted most of `core` before the gate existed | — | none | unestimated | **first half done 2026-08-11** (`bcc3171`): all four facilities evaluated, all four rejected and removed, the rulings recorded in `capability-kernel.md` and `core-reuse.md`. The retroactive import review is open |

`s04` was split and closed only when both halves landed. W2 derives
`identity_hash` and `decision_basis_hash` from the parts; W3 derives the parts.
Neither half was enough on its own: a derived basis over caller-supplied
components still lets a caller pin a snapshot to a world nobody observed, and
composed parts under a caller-supplied hash still let a caller name the
decision. That is why `s04` held a row naming two items until 2026-08-11, and
why W3 had to precede W2 rather than follow it.

W3 carried a prerequisite it does not own. `TaskHost::observe` returns a
`UiObservationSnapshot` whose `canonicalJcs()` is the exact StateResolution
document, and there was no JCS serializer under `modules/task/runtime/` —
`jsonl.luau` was deleted in the runtime rewrite and the trusted resolver returns
frozen Luau tables. Serializing in C++ would mean walking RuntimeModel-derived
structure, which `tests/test-runtime-surface.py` treats as a violation, so it had
to be trusted Luau. **`modules/task/runtime/jcs.luau` landed on 2026-08-10**,
outside these work items, with `tests/task/test-jcs.luau` beside it. The
dependency is closed. One consequence of that landing belongs to whoever
touches it next: `cmake/build.cmake` globs `*.luau`, so `frameworkBundleHash()`
has moved. The second — that the new Luau test ran in no CTest — was closed by
`dcc43b5`, which registered `test-jcs-luau` and made `tests/CMakeLists.txt` fail
configure when any `tests/task/*.luau` is in no `TASK_LUAU_TESTS` entry.

`W3` then `W2` was the keystone: five requirements and three later items hung
off it, and all five closed on 2026-08-11. W4, W6 and W7 are now unblocked.

W1's row said "eight requirements" while its `Closes` column listed six; six is
what it wrote and six is what it closed. The count is corrected above.

W0 ran on 2026-08-10 and split in two. The sanitizer half passed: ASan, UBSan
and TSan each ran 62 of 62 tests with no report, and the activation was proved
rather than assumed — `nm -D` shows the runtime symbols, `ldd` shows the
libraries, and a deliberate heap-buffer-overflow probe did produce a report in
that environment, which is the positive control the result would be worthless
without. `-Wlifetime-safety-permissive`, which `linux-analysis` hard-requires
through `CPP_REQUIRE_CLANG_LIFETIME_SAFETY=ON`, found nothing anywhere.

The `linux-analysis` half failed, and W0's one-day framing was wrong about it.
It is not a toolchain difference to be waived: the run used clang 23.1.0 from
the apt.llvm.org channel CI pins. What it measured on 2026-08-10, before any of
the clearing work, was roughly 14 `-Werror` errors from `-Wunsafe-buffer-usage`,
which `cmake/compiler-safety-analysis.cmake` adds for any Clang, plus about 90
fatal clang-tidy diagnostics. That is real work, and it is W11 above rather than
a footnote on W0.

**W0's two figures are a reading, not W11's scope, and they were already stale
when this plan first quoted them** (recorded 2026-08-10 after
[the third adversarial round](../reviews/2026-08-10-third-round-review.md),
R3-F11). The order matters: `603b0b0` landed before this plan was written and
had already cleared everything outside `modules/operator`,
`modules/task/.../platform` and `contract-suite` — 77 of 106 unique sites over
137 objects, taken to 0. `cec8898` then compiled `modules/task/.../platform`
clean at full `-Werror` under both clang 23 and g++ 15, and `6f8d3a8` cleared
what `7cef402`'s new translation unit made visible. What is left is
`modules/operator`, `contract-suite` and `tests/operator`, and an agent is
clearing it. State the property and the date of the reading; a count written
into a plan is wrong by the next landing, and W0's `90` and `603b0b0`'s `29`
were never shown to count the same thing — one site can emit several
diagnostics, and neither text says which unit it uses.

Three cautions attach to W11. First, `WarningsAsErrors: '*'` combined with C++20
modules means the first failing library kills every downstream BMI: the
CI-faithful run analyzed 92 objects and never reached `modules/task`,
`modules/operator` or `entry/`, so the 90 is a floor and not a total. Any future
claim that clang-analysis passed must state how many objects it analyzed, or it
says almost nothing. Second, the job's header diagnostics were being discarded
outright by an unusable `HeaderFilterRegex` until 2026-08-10 — see
[checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) — so the
diagnostic count will rise again once that filter starts matching.

Third, **this is not a Linux-only item.** `CMakePresets.json` carries an
`x64-analysis` configure preset inheriting `x64-debug` with
`CPP_ENABLE_CLANG_TIDY=ON`, and `cmake/static-analysis.cmake` has an MSVC branch
adding `--extra-arg=/EHsc` precisely so clang-tidy runs there. So the dead header
filter was suppressing diagnostics on every Windows analysis run as well, and
W11's clang-tidy half surfaces on both hosts. Only the `-Wunsafe-buffer-usage`
errors are Clang-specific.

What the three landed items actually left behind:

- **W1** added behavioural bodies to the six cases named above. After W10's
  rename, `test-contract-operator` compiles **39** doctest cases and **32** are
  registered CTests. Prefer that structure to the numbers: the registered set is
  whatever `cpp_add_contract_suite`'s `CASES` list names, and it moves whenever a
  work item adds a gate.

  The gap between 39 and 32 is not slack. **Seven compiled cases run in no CTest
  at all** — the `ProjectPlugin …` cases in
  `tests/operator/test-project-plugin-contract.cpp`, which compile, link and are
  executed by nothing. The cause is an asymmetry between two CMake helpers:
  `cpp_add_contract_suite` builds its binary `NO_CTEST` and registers one test per
  `CASES` entry and nothing else, while `uf_add_operator_contract_suite`
  additionally registers a `contract-suite-<project>` aggregate that runs the
  whole binary. **Ruled: `cpp_add_contract_suite` gains the aggregate**, and
  `dcc43b5` applied it on 2026-08-10: `test-contract-operator` and
  `test-contract-runtime` are now registered CTests under the `CONTRACT-SUITE`
  label, so flipping an assertion in one of the seven turns the suite red while
  every per-case gate stays green.

  That is the fourth instance found on 2026-08-10 of one defect — a name exists,
  the name promises something, and nothing verifies the promise. The other three
  are the dead `HeaderFilterRegex`, `contract-suite/` missing from `SOURCE_ROOTS`
  in the format and safety checkers, and `test-annotate-backend` running in no
  gate. [Checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) records
  them as one family, which is worth more than any of the four separately.
- **W5** implemented the Replay Bundle closure and both publication gates in
  `tools/annotate`, whose suite is 43 tests. The checked-in workspace JSON
  schema file did not change — its SHA-256 is still
  `a6fc31b5e0ee49f5368d66fae3f2abf38e0e58f57d799e3d2cd8da583f508a29`, so the
  C++ pin `k_annotationWorkspaceSchemaHash` is untouched. The Python-side
  workspace SQLite schema root hash did move, to
  `72fa0c39964397921007665e2f4f3f7936bd46f476a3adf589d32bd59ce9d873`, because
  four tables were added (`replay_bundles`, `replay_bundle_blob_refs`,
  `project_operation_replay_intents`, `project_operation_attestations`).
  Nothing in C++ pins that hash. One design consequence is deliberate and
  stricter than the requirement's wording: the checked-in `ReplayGate` marks
  both sub-gates `required`, so publishing a RuntimeModel is now blocked by a
  missing project/operation gate as well as by a missing UI one. "Independent"
  means neither gate's evidence satisfies the other, not that either may be
  absent.

  The third adversarial round confirmed on 2026-08-10 that **the bundle identity
  cannot be steered by a caller**: `_bundle_document` refuses a supplied
  `bundle_id` before anything else, every definition under the workspace schema
  root sets `additionalProperties: false` so no extra field can ride into the
  hash, and `_verify_bundle_file` recomputes the address from the stored bytes
  on every read. One consequence it recorded is a design fact rather than a
  defect: the address covers `frame_retention_expires_at`, so two bundles over
  identical evidence differing by one second of expiry are two permanent rows.
- **W8** made the refcount a set of foreign keys rather than a counter:
  `runtime_installations` rows, a new `runtime_publications` table, and
  `runtime_state.active_runtime_artifact_root_hash`. An artifact directory is
  reclaimable exactly when no row in those three names its root hash. The
  Operator ledger DDL fingerprint therefore changed to
  `5738e6f98534efbdfc3114413de70c032b64e2cbaa84d4c152ec6cbb512120a4`, which
  appears once in the tree, in `modules/operator/source/operator/ledger.cpp`.
  An existing operator database is refused at open rather than migrated;
  nothing is released, so those databases are recreated.

  > **Corrected 2026-08-11: two legs, not three, and a different fingerprint.**
  > The third adversarial round's R3-F2 showed the `runtime_publications` leg
  > was observed by no test and defended by a comment the code did not support.
  > `848e390` deleted the table rather than leaving it under a debt marker,
  > after verifying its unreachability independently — reclamation has no
  > non-test caller, `OperatorCoordinator` appears in no entry point, and
  > `open()` holds `PRAGMA locking_mode=EXCLUSIVE` for the connection's
  > lifetime. The reference set is now `runtime_installations` and
  > `runtime_state.active_runtime_artifact_root_hash`; the W8 property above
  > holds over those two. The fingerprint at that date was
  > `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`
  > over 20 tables. A-F8 stays closed: what closed it was reading the whole
  > reference set at one time, not the number of legs in it.
  >
  > **Current as of the block's close:**
  > `sha256:bda31e4b18a8096b28e5208f5988dea8658bea9d7917d78cd8655d4f581a8559`
  > over 23 tables, still occurring exactly once in the tree, at
  > `modules/operator/source/operator/ledger.cpp`. It moved three more times —
  > `937773366f…` with W4's `dispatches` DDL, `c691f1d9bf…` with W6's two new
  > tables, then this with W7's `agent_budgets` — and W4's move is the one worth
  > remembering, because it changed no table at all. The reference set is
  > untouched by all three.

**W12 is the only item here that came from outside this block's scope, and how
it arrived is the reason it exists.** On 2026-07-25 a simplification sweep
measured `Synchronized`, `ControlFlow`, `Flags` and `NonZero` as included by
exactly one file, `tests/core/test-capabilities.cpp`, and by nothing in
production. It declined to delete them and ruled instead that all four are
unvalidated core surface to be run through `evaluate-core-capability` — a
governance question it put outside its own scope. That ruling is §6 of
[the archived sweep](../archive/reviews/2026-07-25-simplify-sweep.md), and it
reached no TODO entry and no plan. The review was archived on 2026-08-01 in
`eb1d205`, and the ruling was archived with it. Re-measured at `55bd564` on
2026-08-11 the four counts were unchanged and the evaluation had not run:
seventeen days in which the finding was correct, recorded and inert. `CLAUDE.md`
gained an archiving precondition the same day so the next closed document cannot
leave the same way. Do not amend the archived review to match — the pointer runs
one way, from here to there.

**What closes W12 is a recorded answer per facility, not a particular verdict.**
An evaluation was in flight on 2026-08-11, and `ControlFlow` in particular turns
on whether `modules/vision/source/vision/sad.hpp` is a real call site. Keeping a
facility closes its row exactly as well as removing one does; what does not close
it is a second measurement. Each answer has to land where a later reader meets
the facility — the kernel list in
[`core-reuse.md`](../../.claude/skills/cpp-coding/references/core-reuse.md) and
the Core additions table in
[`capability-kernel.md`](../../.claude/skills/evaluate-core-capability/references/capability-kernel.md)
— or the next survey measures the same thing a third time.

That evaluation returned in `bcc3171` on 2026-08-11. All four were **rejected**
and deleted with their capability tests, each on its own grounds rather than on
the caller count: `Flags` has no bitmask-valued enum to serve, `Synchronized`
cannot express the wait the tree's one real mutex needs, `NonZero` had no
candidate, and `ControlFlow`'s one plausible call site — `vision/sad.hpp` —
would have made an invalid state representable. The four rulings are in
`capability-kernel.md` and the kernel list in `core-reuse.md` is corrected, so
they landed where a reader meets the facility rather than only here.
`Synchronized`'s aliasing contract was moved into the `cpp-coding` safety
profile before its file went, which is the only thing any of the four carried
that needed a home.

**The retroactive import review is the larger half, and it explains the shape of
the surface.** 15 of the 23 files under `modules/core/source/core/` at `bcc3171`
came from the repository's first commit, `79e6b3d` (2026-07-20), as a template
import — 19 of 27 before that commit removed four of them. `modules/` held
nothing but `core` at the first commit, and
`evaluate-core-capability` did not exist until `98e0d63` two days later. Every
facility admitted after product code existed has a production caller outside
`core`. So the callerless surface is an **admission** failure rather than an
adoption failure, and the gate works whenever it is actually run. The standing
form of that finding is the **Admission history** section of
`capability-kernel.md`, put there rather than here deliberately: a plan gets
archived, and this item exists because a finding was archived. What W12 still
owes is the one pass over the remaining 15, not a permanent re-litigation.

## 4. Rulings already made

- The schema-shape gates become `schema-*`; `contract-*` is reserved for
  behaviour. Applied incrementally by W10 rather than as one rename, so no gate
  is ever renamed into a promise it does not keep. The split is **per assertion,
  not per case**: project-parameterised store behaviour lives in the exported
  suite and takes the `contract-` name, schema-shape reads stay in
  `tests/operator/` and take the `schema-` name, and a mixed case keeping both is
  honest and is not split further.
- `OperatorCoordinator` grows to hold observation and plan data. A second
  trusted object is a second place authority can leak from.
- **W2 and W3 land as one change; `s04` closes only when both do.** See §3 and
  [the reconciliation](2026-08-10-w2-w7-reconciliation.md), which also carries the
  union of the four items' DDL changes so the schema fingerprint is recomputed
  once per landing rather than once per item. (Executed 2026-08-11 as two
  landings, W3 then W2; the second half of the ruling held and the first was
  departed from. §3 says what that cost and why the hazard did not occur.)
- **`p03`'s containment is declaration plus attribution, not prevention.** The
  Tool Catalog is project-owned, so a project can mark a coordinate tool
  `Semantic`; `plugin_hash` inside `project_registration_hash` makes that
  attributable rather than undetectable. What `p03` enforces is that the Operator
  never offers and never accepts a `Privileged` tool for an online Agent. A
  deployment-principal co-signature over the catalog was considered and
  rejected: two trust models beside `ToolMutability`'s one are worse than one
  documented limit. The limit is stated at the `ToolSurface` declaration.
- Artifact GC is by database refcount, not mark-and-sweep. A sweep must decide
  what "orphan" means while a concurrent publisher is mid-install, which is the
  hazard behind finding A-F8. (W8 executed this on 2026-08-10 and A-F8 moved
  from accepted to closed; see
  [the review outcome](../reviews/2026-08-10-runtime-hardening-review.md).)

## 5. Outside these work items

- **Publishing a consumable contract suite — done 2026-08-10.** It was raised
  here as unowned; it is no longer. `cmake/operator-contract-suite.cmake`
  exports `uf_add_operator_contract_suite()`, and
  `contract-suite/include/operator-contract/project-under-test.hpp` is the
  single public header a consumer implements: it supplies a
  `ProjectUnderTest` — its registration, plugin and a vocabulary of tool and
  Journal documents its own schemas accept — and the suite invents no project
  bytes of its own. The suite's translation units compile into the consumer's
  executable rather than shipping as a library, so a run carries the consumer's
  safety profile and sanitizers. Two structurally unrelated fixtures under
  `contract-suite/fixtures/` exercise it, each written the way a consuming
  repository writes its own: one `CMakeLists.txt` calling the function and one
  provider translation unit. They register as `contract-suite-umbraflow` and
  `contract-suite-arcana`. This unblocks Phase 2C; it does not satisfy the
  dual-game gate below, which still needs two real, independently owned
  registrations.
- Phases 2B, 2C, 3, 4 and anything in a consumer repository.
- The real dual-game attestation: `EXTERNAL / NOT_RUN`, unmovable by fixtures.
  Since 2026-08-11 the nine consumer attestations `attest-consumer-d01`-`d09`
  have a specified shape and recording location —
  [consumer attestation](2026-08-11-consumer-attestation.md) — which rules them
  **independent of this gate in both directions**: a complete attestation set
  does not move `NOT_RUN`, and `NOT_RUN` does not block eight of the nine. That
  document leaves `attest-dual-game-p05` itself unspecified and says why: it is
  a fact about two independently owned consumers, which no single consumer can
  attest.
- Production `click`, `key`, `drag`, `run`: closed by design, and still closed
  by construction — nothing outside a test calls `Host::deliver`.
- Accepted finding B-F4, which stays accepted. A-F8 was also accepted when this
  plan was written; W8 closed it on 2026-08-10.

## 6. What this plan achieved, and what is left

**Achieved.** All 20 requirements that were open when this plan was written are
implemented, and 18 of them gained a behavioural gate: W1 took six, W5 two, W3
two, W2 three, W4 two, W6 three and W7 two. The remaining two are `a03` and
`a05` — implemented by W5, gated by an aggregate, short only a per-requirement
ID. So all 42 `REQUIRED_CORE` requirements are implemented and 40 own a gate
that goes red when the behaviour is removed. W10 finished alongside them, so
`contract-*` now means exactly what it says in both directions — no gate
overstates, and none understates either. The exported contract suite (§5)
shipped, so a consuming repository can run the store contracts against its own
project. Along the way the block found and recorded eight instances of one
defect — a name that promises something nothing verifies — three of them inside
the machinery built to catch the rest; it ran every falsifying mutation four
specifications had written and never executed; and it came out naming its
unfalsifiable properties rather than counting them as coverage (§2).

**Open, in the order that matters.**

1. **W11 — `clang-analysis`.** It blocks the branch rather than the design:
   `linux-analysis` is a required CI job and has never been seen to pass.
   Several landings advanced it and none finished it. Done when a
   `linux-analysis` build reports nothing **and states how many objects it
   analysed**; a pass without a denominator is the shape §2's pitfall document
   is about.
2. **W9 — the review round over W2-W7.** Unblocked for the first time, and
   nothing has reviewed the five landings that carry the block's actual
   requirement closures. The 2026-08-10 round ended at `cec8898`, before W2.
3. **W12's second half** — the retroactive review of the 15 files that entered
   `core` in the 2026-07-20 template import, before `evaluate-core-capability`
   existed. The first half landed in `bcc3171`.
4. **Per-requirement IDs for `a03` and `a05`** — naming work, described at the
   end of §2. It is the only remaining place where a gate name says less than
   the tree does.
5. **The unresolved test defects and open questions this block did not close.**
   W2's `T2`, `T10` and `T13` stay green; `maximum_observations`,
   `maximum_waits` and `maximum_elapsed_ms` are clamped and never compared;
   `StepKind::Wait` is unexercised; `ApprovalRequest::policyHash` is still a
   caller field. The reconciliation's §8 still carries `k_workflowCeiling`'s
   invented values, `required_approvals` as 0/1, and the
   `controlled_target_id` / `controlled_target_key` split — one concept, two
   spellings, in the schema and the code respectively, predating this block and
   still unfixed.

**Not this repository's to close, and a different stage.** The consumer-side
work is specified in
[consumer attestation](2026-08-11-consumer-attestation.md), whose §10 holds six
questions awaiting a ruling; the real dual-game attestation is `EXTERNAL /
NOT_RUN` and unmovable by fixtures. Neither blocks anything above.

The item that used to stand here — that the 19 `schema-*` gates were
unfalsified — was closed on 2026-08-10 by the third adversarial round; see §1.

## 7. Corrections to this block's record, 2026-08-10

The finding record is
[the third adversarial round](../reviews/2026-08-10-third-round-review.md). What
follows are the dispositions that belong in the plan rather than in the review,
and one correction to the review itself. Nothing landed is rewritten to match.

**`dcc43b5`'s "cannot be split" does not hold, and the real coupling is worse
than the one it claims.** The commit message gives one reason for a single
landing: `tests/CMakeLists.txt` enforces that registered case names exactly
match the declared `TEST_CASE`s, so a split would leave an intermediate commit
where configure fails for the whole tree. R3-F6 is right that this does not
reach the `scripts/check_safety.py` half — renaming ten rules from
`ADR-011 forbidden ...` to `background_only forbidden ...`, dropping `external`
from `UNSAFE_DIRECTORY_NAMES`, and adding
`test_a_vendored_directory_is_never_a_boundary_directory` touch no `TEST_CASE`
name, no CMake list and no CTest registration. Only
`SOURCE_ROOTS += "contract-suite"` in the two Python gates needed the new
directory to exist.

R3-F6 is wrong about the artifact-reclamation half, and the correction is the
more useful fact. The finding argues that half "already stands alone" at
`dcc43b5` because its only dependency, `ConfinedRoot::removeTree`, arrives later
in `cec8898`. The dependency runs the other way. At `dcc43b5`,
`modules/operator/source/operator/ledger.cpp` calls `removeTree` and
`childNames` on a `task_platform::ConfinedRoot`, and neither member exists
anywhere in the tree at that commit: `git grep removeTree dcc43b5` returns three
call sites in that one file and no declaration. **`dcc43b5` does not compile**,
and `cec8898` is what makes it build. So the block already contains a broken
intermediate commit — a build failure rather than the configure failure the
message was guarding against — and the stated reason for the single landing did
not prevent it. Whoever recombines this history before it is published should
fold the `ConfinedRoot` extension into `dcc43b5`, or order `cec8898` first. That
is a note for a quiet tree, not a licence to operate on history while other
agents hold work in it.

**`f0b351b`'s closing sentence names a lane that does not run.** "Removing one
suppression turns the gate red" is true of clang-tidy and false of any gate a
developer runs: clang-tidy is enabled only by `CPP_ENABLE_CLANG_TIDY`, which
belongs to the three `*-analysis` presets and the `clang-analysis` CI job, and
`scripts/ci-local.*` configures the host debug preset. The same block's
[coding standard amendment](../../.claude/skills/cpp-coding/references/coding-standard.md)
records that the job does not compile. The claim is about a lane that is red for
other reasons, which is W11 above.

**Verified negatives worth not re-deriving.** The 11
`cppcoreguidelines-pro-type-member-init` suppressions that stood at `cec8898`
are each correct: only four member types are involved — `PixelRect`,
`PixelPoint`, `FrameId` and `ContentHash` — and none is default-constructible,
so the check's suggested `{}` fix would not compile. `6f8d3a8` added three more
by the same argument, proved the same way and with the probe run in reverse
first. Two limits survive and are not defects: the proof was never committed as
a `static_assert`, so a default constructor added to one of those types later
turns the suppressions into silent holes; and `NOLINTNEXTLINE` sits above the
record, not the member, so a bare new field in one of those structs is swallowed
without a diagnostic. `scripts/member_init.py`, which is in the local gate, sees
none of these members because it matches only `m_\w+` names.
