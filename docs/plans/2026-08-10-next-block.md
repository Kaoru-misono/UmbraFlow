# The next block after runtime hardening

Status: proposed; no code changed on its account
Date: 2026-08-10
Scope: `umbraflow-cpp` only. No consumer-project writes.

## 0. The bundle moved twice, and is re-pinned

The consumer bundle went 1.7 to 1.8 and then to 1.9 during this session. Every
correction was to the project-layer document; the upstream design changed only
its version string, and its contract text is byte-identical at unchanged line
numbers at all three versions, which is what shows each bump replaced text in
place rather than moving anything.

1.8 corrected the two divergences the consumer session had found: C0-C1 were
behind a gate upstream §15 marks as parallel work, and §13's four validation
levels had no mapping onto the project's C0-C4 axis. 1.9 corrected six more
found while making those, the load-bearing one being that §11 C3 had dropped
upstream Phase 4's precondition — a reader following the project document alone
would open the first real mutation without the dual-game gate.

Current root `c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`.
The requirement diff the handoff demands on a hash change was done at each bump,
not assumed. This plan assumes 1.7 phase semantics and that assumption survives:
only the label moved.

## 1. What is genuinely finished

Each line is a command or an artifact, not a claim.

| Finished | Evidence |
|---|---|
| Full local gate on Windows | `scripts/ci-local.ps1` prints `GATE: PASS` |
| 61 CI tests, 43 contract gates | `ctest -N`; a mismatch against the migration report is a configure-time `FATAL_ERROR` |
| Two adversarial review rounds, every finding closed or accepted | `docs/reviews/2026-08-10-runtime-hardening-review.md` |
| Production mutation closed **by construction**, not by a flag | `Host::deliver` has one caller in the tree, a test; the CLI registers only `explore` and `targets` |
| Journal and reducer have one source | the Operator builds the reduce envelope itself; `ReconciliationCommit` carries no reducer input |
| Caller cannot downgrade mutability or relabel a disposition | `ValidatedToolInvocation`, `ValidatedReconcileOutcome`, and the three tests that attack them |
| Handle-based path confinement | `task/platform/confined-file`, plus the repository's first reparse coverage |

**Not finished, and the documents read as if it were.** Roughly half the
requirement gates assert JSON-Schema definition shape rather than behaviour:
every `contract-state-s01..s06`, every `contract-agent-a01..a08`, and
`contract-control-c02,c03,c04,c05,c07,c08` read a schema file and check that a
definition exists with certain members. The behavioural gates are `c01`, `c06`,
`c09`-`c14` and `p04`. A shape check under a requirement ID reads as coverage
the requirement does not have. See decision 4.

## 2. CI: never run on this branch

`design/annotation-system-v2` has never been pushed — no matching ref under
`refs/remotes`, and `.github/workflows/ci.yml` triggers on push only for
`master`, plus pull requests. So Linux GCC, Linux Release, Windows Release,
macOS, clang-tidy, ASan, UBSan and TSan have never seen this tree. Billing is
not the question on this branch; the branch simply never left the machine.

CI is deliberately restricted to `master`, and this repository is a topic branch
that merges back. So there is no pre-merge CI at all: the first time eight
configurations see this work is on `master`, after the merge. That makes a red
`master` the default outcome unless the packaging below is fixed first, and it
makes local non-Windows verification the only pre-merge signal there is.

**A dependency this session introduced is undeclared.** `jsonschema` is
imported by `tools/annotate/contracts.py` and the annotation tests, the
repository has no `requirements.txt`, `pyproject.toml` or `setup.py`, and
`ci.yml` never installed it. `test-annotate-backend` now carries the `CI` label,
so the first run would have failed in every job that runs `ctest -L CI` — caused
by a change made to close a fake-green finding. Fixed here: `requirements.txt`
declares it and every job installs it after `setup-python`.

## 3. The next phase boundary

**This tree is mid Phase 2A, not past it**, despite being green. §15 Phase 2A
names `ToolDescriptor, ToolInvocation, CommandRecord, EffectivePlan, Operation,
ToolResult` and `ControlLease, Snapshot Coordinator, Host atomic fencing`.

Present: `ToolInvocation`, `ToolDescriptor`, `Operation`, `ControlLease`, a
durable ledger, idempotency, the mutation chain, approvals, reconciliation.

Absent:

- `EffectivePlan`, `PlanProposal` and `ToolResult` exist only as JSON-Schema
  definitions. No C++ symbol exists for any of them.
- No Snapshot Coordinator. `createSnapshot(lease, identityHash)` takes the
  identity as a caller argument, and `SnapshotRecord` carries no observation and
  no project state.
- Host fencing is not joined to the ledger. `recordDeliveryOutcome` takes a
  `DeliveryOutcome` enum from the caller; nothing returns one from
  `Host::deliver`.
- Four of the five plugin functions have no trusted invoker. Only `reduce` is
  called by the Operator; `derive`, `plan`, `next_step` and `reconcile` are
  called from tests only.
- Policy is stored, never evaluated. `policy_hash` is a column,
  `ApprovalRequest` takes `policyHash` and `effectEnvelopeHash` from the caller,
  and `schema/umbraflow-policy-v1.schema.json` has no reader.

Phase 2A's own gate names three tests. None exists in the form it asks for: the
takeover race test covers lease fencing and stale snapshots but has no dispatch
in the critical section; caller **effect** downgrade is untestable because there
is no EffectivePlan to downgrade; there is no cross-restart lease-epoch test,
and `recoverUncertainDispatches()` has no test caller at all.

**The next block is one theme: close the caller-asserted authority seam in
Operator Core** — every field that decides what may be dispatched is derived by
the Operator rather than supplied by its caller. That is the same move made
three times already this session for the reducer input, tool mutability and the
reconciliation disposition; what remains is the plan, the policy and the
delivery outcome.

## 4. What would open production mutation

Closed today because nothing outside a test calls `Host::deliver`. The sequence,
in order, no step skippable:

1. Finish Phase 2A (§3) — gate G2.
2. **Publish a consumable ProjectPlugin contract suite.** Nobody owns this yet.
   The suite is `tests/operator/*.cpp` compiled inside this repository; there is
   no exported harness a consumer repo can run against its own registration.
   Until it exists the external gate cannot start.
3. Phase 2C — the second game registers a real plugin and passes the offline
   mutation fixture. Needs step 2.
4. Phase 3 — the Chaos read-only observer.
5. **G3, the real dual-game attestation.** Records two exact
   `project_registration_hash` values from two independently owned
   repositories. `EXTERNAL / NOT_RUN`; no fixture can satisfy it.
6. Phase 4 / G4 — first mutating loop, whose precondition is that both real
   plugins passed the same suite and neither project has opened mutation.

Upstream must build, before step 3 can begin: the EffectivePlan/policy/ToolResult
authority, the Operator-owned Snapshot Coordinator that calls `derive`,
`Host::deliver` joined to dispatch reservation and outcome, and an exportable
suite plus a stable registration manifest.

## 5. What the consumer is waiting on

**Phase 2B, C0 and C1: nothing. Upstream owes them zero.** They consume only the
client's raw database — not Operator contracts, not Runtime v2 output. The 1.8
correction exists precisely because a document had scheduled them behind an
upstream gate. If uf-chaos is frozen pending upstream, C0-C1 are frozen for no
reason, and that is worth telling the owner today rather than later.

**C2 waits on four named artifacts**, not on readiness:

| C2 needs | Upstream state |
|---|---|
| Runtime v2 model and artifact schema, and an authoring path that publishes a model | Exists; never exercised from outside this repository |
| the Operator `derive` boundary | Declared, never invoked by the Operator. Blocking |
| `availability`, `provision` | `provisionProjectInstance` exists; `availability` does not. Blocking |
| `UIActionIntent` / `WaitIntent` data boundary | Does not exist in any form. Blocking |

**C3** waits on the whole of §4.

## 6. Deferred engineering, ordered by dependency

| # | Item | Why it matters | Depends on | Cost |
|---|---|---|---|---|
| D1 | ~~Declare `jsonschema` and install it in CI~~ done. What remains: verify Linux, macOS and clang-tidy locally before merging, because CI runs only on `master` and therefore only after the merge | Eight configurations have never compiled this tree; `-Werror` on GCC already caught one dead reader the Windows-only gate could not see, and `linux-analysis` is the only checker for parts of the ownership rules | none | 1 day |
| D2 | **EffectivePlan authority**: mint it from a plugin `PlanProposal` bound to registration, command fingerprint and decision basis; derive the frozen plan, step intent and effect envelope hashes from it; `reserveDispatch` takes the minted plan instead of three caller hashes | Effect, risk, scope and workflow bounds are caller assertions today. Closes the Phase 2A effect-downgrade gate | none | 3-5 days, the largest item |
| D3 | **Reconcile input assembled by the Operator** | `plugin.reconcile` has no trusted caller, so its input is whatever the caller built. The disposition is protected; the question it answers is not | D2 — the observation and plan data it lacks arrives there | 2 days after D2 |
| D4 | **Snapshot Coordinator**: compose UI observation, `plugin.derive` and current ProjectState atomically, and derive the identity hash rather than accept one | S-01/S-02 are asserted by shape only; also unblocks consumer C2 | D2 | 3 days |
| D5 | **Join Host delivery to the ledger**: `recordDeliveryOutcome` takes what `Host::deliver` returned, inside the fence | The dispatch row and the delivery are two unjoined worlds. Enables the real check-then-click takeover race test | D2, D4 | 2-3 days |
| D6 | Cross-restart lease-epoch test, and a caller for `recoverUncertainDispatches()` | Named by the Phase 2A gate; the recovery entry point is unexercised | partial form startable now | 1 day |
| D7 | Artifact GC for orphaned `runtime-artifacts/<hash>/` and an empty `.staging` | Unbounded growth, no correctness impact | none — independently schedulable | 1-2 days |
| D8 | Third review round | Rounds 1 and 2 each found regressions caused by the previous round. A third over an unchanged tree has low yield; over D2-D5 it has high yield | D2-D5 | 1 day per reviewer |

## 7. Decisions for the owner

Decisions 3, 4 and 5 below were ruled by the owner on 2026-08-10 and are
recorded as settled. 1 and 2 resolved themselves.

1. Settled: `jsonschema` is declared in `requirements.txt` and installed by
   every job.
2. Settled by the CI policy: CI runs on `master` only, so the branch is
   verified locally and then merged. There is no pre-merge CI to push for.
3. **Ruled: rename the schema-shape gates `schema-*`** and reserve `contract-*`
   for behaviour. The migration report promises owner, schema and test per
   requirement; today the third column overstates. This edits the report's ID
   list and the CMake enforcement list.
4. **Ruled: `OperatorCoordinator` grows** to hold observation and plan data
   rather than a separate Session Coordinator. A second trusted object is a
   second place authority can leak from.
5. **Ruled: artifact GC by database refcount**, not mark-and-sweep. A sweep has
   to decide what "orphan" means while a concurrent publisher is mid-install,
   which is the hazard behind accepted finding A-F8.
6. **Third review round now, or after the block?** Recommend after.

## 8. Deliberately not in this plan

- Phases 2B, 2C, 3, 4, 5, 6, and anything in a consumer repository.
- The real dual-game attestation. `EXTERNAL / NOT_RUN`; no fixture moves it.
- Production `click`, `key`, `drag`, `run`. Closed by design and still closed by
  construction.
- Agent subscription, budgets and anti-loop. Phase 5.
- Re-litigating the accepted findings A-F8 and B-F4.
