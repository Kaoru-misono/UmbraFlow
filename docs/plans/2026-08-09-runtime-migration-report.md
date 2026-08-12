# Runtime v2 upstream migration report

Status: G0 execution map
Date: 2026-08-09
Scope: `umbraflow-cpp` only; no consumer-project writes

This report binds the v1.12 spec root
`b3306dde9337a70e5e33bb5676f9da5b0e99b4b1acd2fec1ef4d16dbde51cda5`
to the inherited upstream baseline rooted at
`55444b02a8ace9fe7493e5175618ef0a67d87402087874b7972187ac71ed8ac7`.

> Amended 2026-08-12: written against v1.9, root `c4760bb5…bfb6a966`, and
> re-bound here because G0 in the requirements matrix passes only on a migration
> report bound to the *current* bundle root. The bundle moved on 2026-08-12
> through v1.10 and v1.11 to v1.12; no requirement row, owner, schema path or
> CTest ID below changed with it.

The executable specification resolutions derived from the product bundle are
fixed by
[`2026-08-09-runtime-hardening-rewrite.md`](2026-08-09-runtime-hardening-rewrite.md).

## Executable schema locations

These are target paths, not claims that the current dirty implementation is
complete:

| Code | Checked-in authority |
|---|---|
| RA | `schema/umbraflow-runtime-artifact-v1.schema.json` |
| RM | `schema/umbraflow-runtime-v2.schema.json` |
| PR | `schema/umbraflow-project-registration-v1.schema.json` |
| OP | `schema/umbraflow-operator-v1.schema.json` |
| PL | `schema/umbraflow-policy-v1.schema.json` |
| JR | `schema/umbraflow-journal-v1.schema.json` |
| AW | `schema/umbraflow-annotation-workspace-v2.schema.json` |
| TR | `schema/umbraflow-trace-v2.schema.json` |
| CP | consumer schema hashes referenced by PR; core stores and validates opaque payloads and never defines a game schema |

Verification IDs marked `CTEST` are exact local CTest names. One executable
may serve multiple IDs, but `ctest -N` must list each of those IDs separately.
IDs marked `EXTERNAL` are consumer or cross-repository attestations; local
fixtures cannot satisfy them.

> **Amended 2026-08-10: gates carry two prefixes, and this map was rebuilt from
> the registrations.** `dcc43b5` split the vocabulary — `contract-<area>-<id>`
> exercises the code and goes red when the behaviour is removed;
> `schema-<area>-<id>` only reads a `schema/*.json` file and asserts a
> definition exists with certain members, so it passes whether or not the
> behaviour does. Fourteen rows below moved to the `schema-` spelling. Five more
> requirements, `C-09` through `C-13`, kept a `contract-` gate — it moved into
> the exported suite with the store behaviour it names — and gained a `schema-`
> gate beside it in `tests/operator/`, which is why 42 requirements carried 47
> gates on that date: 28 `contract-*` under label `CI;CONTRACT` and 19
> `schema-*` under `CI;SCHEMA`. (Five more requirements joined them by
> 2026-08-11; see the amendment below for the current totals.)
> `dcc43b5` did not update this report and `5bb281d` edited it
> without correcting the rows; the drift was found by
> [the third adversarial round](../archive/reviews/2026-08-10-third-round-review.md),
> R3-F3. The registrations are the authority:
> `UF_REQUIRED_DOCTEST_CONTRACTS` in `tests/CMakeLists.txt` and the `CASES`
> lists in `tests/CMakeLists.txt` and
> `conformance/exemplars/umbraflow/CMakeLists.txt`.

> **Amended 2026-08-11: five requirements gained a `contract-` gate, so the
> totals are 52 gates over 42 requirements — 33 `contract-*` and 19
> `schema-*`.** The deciding artifact is `UF_REQUIRED_DOCTEST_CONTRACTS` in
> `tests/CMakeLists.txt`, which is machine-checked and was correct throughout;
> this report is the human authority and no gate reads it, which is why it is
> the side that drifted. `4b955de` added `contract-state-s01` and
> `contract-state-s02` with the Snapshot Coordinator; `848e390` added
> `contract-control-c05`, `contract-control-c08` and `contract-state-s04` with
> the EffectivePlan. Neither commit updated the rows below, which is the same
> violation of stop condition 2 that R3-F3 found in `dcc43b5`, and the second
> time the order "report first, then `tests/CMakeLists.txt`" was inverted. The
> five requirements now own one gate of each kind, for the reason `C-09`
> through `C-13` already do: the behaviour and the schema shape are different
> things to guard.

> **Amended 2026-08-11: the block closed, and seven more requirements gained a
> `contract-` gate. The totals are 59 gates over 42 requirements — 40
> `contract-*` and 19 `schema-*`.** The deciding artifact is again
> `UF_REQUIRED_DOCTEST_CONTRACTS` in `tests/CMakeLists.txt`. `25f57f9` (W4)
> added `contract-control-c03` and `contract-agent-a07`; `93698b4` (W6) added
> `contract-product-p01`, `p02` and `p03`; `c23efd3` (W7) added
> `contract-agent-a01` and `contract-agent-a02`. Their rows below are updated to
> name both gates, as the twelve before them are.
>
> **Corrected 2026-08-11 (`07abc3e`): the gate totals stand, the closure count
> does not.** 59 gates over 42 requirements is right, and all 40 `contract-*`
> gates are registered and green. What the paragraph below asserts and should
> not is that a `contract-` gate per requirement means the requirement is
> closed. `contract-agent-a07` proves one of `A-07`'s two acceptance clauses —
> an in-flight dispatch is explicitly reported — and nothing implements the
> other. So **39 requirements own a behavioural gate that closes them, not 40**,
> and `A-07` is reopened; its row below says so. This table maps requirements to
> gates and is correct as such. It is not a closure ledger, and reading it as one
> is how the row stayed unqualified. See
> [the next block](2026-08-10-next-block.md) §2 and §6.1.

> **Corrected again 2026-08-11 (`bed456f`): the correction above also misread
> the acceptance text.** The frozen bundle's row places both consequences
> inside `A-07`'s 验收 — "takeover 返回后旧 fence 不可开始新 dispatch；在途
> dispatch 被明确报告" — not one in its 需求 and one in its 验收. The first
> clause is `reserveDispatch`'s live-lease predicate, `requireLiveLease`, which
> already ran inside the same `BEGIN IMMEDIATE` serialization `takeoverLease`
> commits in, before the reopening; what was missing was a test that ran the
> schedule, not a call edge. `contract-agent-a07` was extended, not joined by a
> second case, and both clauses are now falsified by mutation. **All 40
> `contract-*` gates close their requirement in full, and 42 of 42 requirements
> are closed by a behavioural gate.** `A-07`'s row below is corrected to match.
> See [the next block](2026-08-10-next-block.md) §2.

> **Forty of the forty-two requirements now own a behavioural gate, and the two
> that do not are not open work.** `A-03` and `A-05` are implemented — the
> Replay Bundle closure and both publication gates landed in `25520a3` on the
> Python side — and they are gated, by the aggregate CTest
> `test-annotate-backend`. What they lack is a *per-requirement CTest ID* for
> the behavioural half, not a gate and not the behaviour. A reader who sees
> "40 of 42" and concludes two requirements are unimplemented has the wrong
> conclusion; `UF_SCHEMA_ONLY_REQUIREMENTS` names exactly those two and CMake
> refuses to let the set drift.
>
> **Every commit that has ever registered a gate inverted this report's stop
> condition 2, and that is now the rule's whole history.** `dcc43b5` (found by
> R3-F3), then `4b955de` and `848e390`, then `25f57f9`, `93698b4` and
> `c23efd3`: six commits, one direction, no counter-example. (`e64c143`, W4's
> additive half, registered no gate and owed this report nothing.) The cause is
> structural rather than individual. `tests/CMakeLists.txt` is machine-checked and refuses to disagree
> with itself; **no gate reads this file**, so the ordering "report first" is
> enforced by nothing and the report is always the side that drifts. Stating it
> here rather than only in each landing's message: if this keeps recurring, the
> answer is a check that reads the rows below, not a stricter reading of stop
> condition 2.

> **Amended 2026-08-11: the nine `attest-consumer-dNN` IDs now have a
> specification.** Until that date the `D-01`-`D-09` rows below were the only
> occurrence of those strings anywhere in the tree, and nothing said what a
> consumer produces, who signs it, where it is recorded, or what refuses it.
> [Consumer attestation](2026-08-11-consumer-attestation.md) settles all four:
> one exact JCS `ProjectAttestationSet` inlining nine content-addressed
> attestations, carried as the `attestations` entry of
> `project_artifact_roots` — which is why the `Schema location` column of those
> rows is correct as written and needs no change. The rows stay `EXTERNAL`, and
> that document deliberately adds no upstream gate, because a local fixture
> could satisfy any gate it added. It also records two matrix facts this report
> does not carry: `D-09` is `PHASED` rather than `PROJECT_CONTRACT`, and `C-11`
> and `A-04` carry `PROJECT_CONTRACT` beside `REQUIRED_CORE`, so the consumer's
> obligation list is eleven requirements rather than these nine.

## Requirement ownership and test map

| ID | Owner | Schema location | Verification |
|---|---|---|---|
| P-01 | Operator facade | OP:`ToolInvocation/CommandRecord/Operation` | CTEST `contract-product-p01` and CTEST `schema-product-p01` |
| P-02 | Host + Operator | OP:`ControlTransition/ExternalInputFinding` | CTEST `contract-product-p02` and CTEST `schema-product-p02` |
| P-03 | Script sandbox | OP:`ControllerCapability` | CTEST `contract-product-p03` and CTEST `schema-product-p03` |
| P-04 | Project registry | PR:`ProjectRegistrationManifest` | CTEST `contract-product-p04` |
| P-05 | Upstream harness + real consumers | PR + CP | CTEST `contract-product-p05-fixtures`; EXTERNAL `attest-dual-game-p05` |
| P-06 | Session coordinator | PR + OP:`OperatorSession` | CTEST `contract-product-p06` |
| D-01 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d01` |
| D-02 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d02` |
| D-03 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d03` |
| D-04 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d04` |
| D-05 | Consumer build + upstream root verifier | PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d05` |
| D-06 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d06` |
| D-07 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d07` |
| D-08 | Consumer plugin | OP:`ExpectedEffect/ObservedOutcome` + CP | EXTERNAL `attest-consumer-d08` |
| D-09 | Consumer plugin | CP via PR:`project_artifact_roots` | EXTERNAL `attest-consumer-d09` |
| U-01 | Host + trusted Runtime | RA + RM | CTEST `contract-runtime-u01` |
| U-02 | Host binding | RA:`RuntimeArtifactManifest` + OP:`RuntimeModelBindingRef` | CTEST `contract-runtime-u02` |
| U-03 | trusted Runtime | RM:`UiTarget/Binding/StateResolution/BindingResolution` | CTEST `contract-runtime-u03` |
| U-04 | trusted Runtime | RM:`Evidence/Geometry` | CTEST `contract-runtime-u04` |
| U-05 | trusted Runtime | RM:`Evidence/StateResolution` | CTEST `contract-runtime-u05` |
| U-06 | Host | OP:`ReceiptRef/DeliveryOutcome` | CTEST `contract-runtime-u06` |
| U-07 | Annotation + deployment boundary | AW:`Publication` + RA | CTEST `contract-runtime-u07` |
| U-08 | Host + deployment boundary | RA + TR | CTEST `contract-runtime-u08` |
| S-01 | Snapshot coordinator | OP:`SnapshotParts` + JR:`ProjectState` | CTEST `contract-state-s01` and CTEST `schema-state-s01` |
| S-02 | Snapshot coordinator | OP:`ProjectSnapshot/SnapshotToken` | CTEST `contract-state-s02` and CTEST `schema-state-s02` |
| S-03 | Snapshot coordinator | OP:`SnapshotToken` | CTEST `contract-state-s03` |
| S-04 | Operator planner | OP:`DecisionBasis` | CTEST `contract-state-s04` and CTEST `schema-state-s04` |
| S-05 | Session coordinator | OP:`SessionManifest` + PR | CTEST `contract-state-s05` |
| S-06 | Project state store | JR:`ProjectInstance/ProjectState` | CTEST `contract-state-s06` — since 2026-08-11 it also binds the `project_state` column set to `JR:ProjectState`'s `required` list, with `project_instances` versus `JR:ProjectInstance` as the positive control that the comparison can fail; see [the journal record binding](../archive/plans/2026-08-11-journal-record-binding.md) |
| C-01 | Host control ledger | OP:`ControlLease/FencingToken` | CTEST `contract-control-c01` |
| C-02 | Host control ledger | OP:`SessionEpoch/ControlLease` | CTEST `contract-control-c02` |
| C-03 | Host delivery | OP:`DeliveryAuthority/ReceiptRef` | CTEST `contract-control-c03` and CTEST `schema-control-c03` |
| C-04 | Operator facade | OP:`ToolInvocation/CommandRecord` | CTEST `contract-control-c04` |
| C-05 | Operator planner | OP:`PlanProposal/EffectivePlan` + PR | CTEST `contract-control-c05` and CTEST `schema-control-c05` |
| C-06 | Operation ledger | OP:`CommandRecord/Operation` | CTEST `contract-control-c06` |
| C-07 | Operation ledger | OP:`OperationState/PlanVersion` | CTEST `contract-control-c07` |
| C-08 | Operator planner | OP:`EffectivePlan/UIActionIntent/WorkflowLimits` | CTEST `contract-control-c08` and CTEST `schema-control-c08` |
| C-09 | Operation ledger | OP:`DispatchOutcome/ToolResult` | CTEST `contract-control-c09` and CTEST `schema-control-c09` |
| C-10 | Operation ledger + Host | OP:`DispatchRecord/DeliveryOutcome` | CTEST `contract-control-c10` and CTEST `schema-control-c10` |
| C-11 | Reconciliation coordinator | OP:`ReconcileProposal` + JR | CTEST `contract-control-c11` and CTEST `schema-control-c11` |
| C-12 | Policy/approval ledger | PL + OP:`ApprovalToken/AuthorityDecision` | CTEST `contract-control-c12` and CTEST `schema-control-c12` |
| C-13 | Operation ledger | OP:`MutationChain` | CTEST `contract-control-c13` and CTEST `schema-control-c13` |
| C-14 | Operation ledger | OP:`DispatchRecord/OperationState` | CTEST `contract-control-c14` |
| A-01 | Agent event facade | OP:`SubscriptionCursor/ResyncRequired` | CTEST `contract-agent-a01` and CTEST `schema-agent-a01` |
| A-02 | Agent runtime | OP:`AgentBudget/ProgressMarker` | CTEST `contract-agent-a02` and CTEST `schema-agent-a02` |
| A-03 | Audit owners | TR + OP + JR + AW:`ReplayBundle` | CTEST `schema-agent-a03`; behaviour under the aggregate CTEST `test-annotate-backend`, with no per-requirement ID |
| A-04 | Reconciliation coordinator | JR:`JournalEvent` | CTEST `contract-agent-a04` — since 2026-08-11 it also binds the `journal_events` column set to `JR:JournalEvent`'s `required` list and drives six provenance documents that each violate one rule of the fixed `JR:JournalProvenance`, which the framework now enforces itself; see [the journal record binding](../archive/plans/2026-08-11-journal-record-binding.md) |
| A-05 | Publication gates | AW:`ReplayGate` + PR:`plugin_hash` | CTEST `schema-agent-a05`; behaviour under the aggregate CTEST `test-annotate-backend`, with no per-requirement ID |
| A-06 | Deployment boundary | AW:`AuthoringCapabilityRoot` + RA | CTEST `contract-agent-a06` |
| A-07 | Host control ledger | OP:`ControlTransition/DeliveryAuthority` | CTEST `contract-agent-a07` and CTEST `schema-agent-a07` — **closed**: `contract-agent-a07` proves both acceptance clauses — the displaced lease is refused a reservation the live lease is then granted, and a second Host still carrying the displaced fence cannot deliver it — after being extended 2026-08-11 (`bed456f`) to run the schedule the first clause needed. Reopened earlier the same day (`07abc3e`) on a misreading that substituted the 需求 sentence for the first 验收 clause. See [the next block](2026-08-10-next-block.md) §2 |
| A-08 | Operator recovery | OP:`ExternalInputFinding/OperationState` | CTEST `contract-agent-a08` |

Where the 59 gates are declared, as of 2026-08-11:

- `tests/operator/test-product-contract.cpp` — `schema-product-p01`-`p03` and
  `contract-product-p01`-`p04`, `p06`;
- `tests/operator/test-project-plugin-contract.cpp` —
  `contract-product-p05-fixtures`, plus the generic opaque-payload isolation
  cases, which carry prose names and run only under the aggregate;
- `tests/task/test-runtime-v2-contract.cpp` plus trusted Luau fixtures —
  `contract-runtime-u01`-`u08`, plus the Host delivery and fence cases, which
  carry prose names and run only under the aggregate;
- `tests/operator/test-state-contract.cpp` — `schema-state-s01`, `s02`, `s04`
  and `contract-state-s01`, `s02`, `s03`, `s04`, `s05`, `s06`;
- `tests/operator/test-control-contract.cpp` — `contract-control-c02`, `c03`,
  `c04`, `c05`, `c07`, `c08`, `c14` and `schema-control-c03`, `c05`, `c08`,
  `c09`-`c13`;
- `tests/operator/test-agent-audit-contract.cpp` — `contract-agent-a01`, `a02`,
  `a04`, `a06`, `a07`, `a08` and `schema-agent-a01`, `a02`, `a03`, `a05`,
  `a07`;
- `conformance/source/suite-control-ledger.cpp` — `contract-control-c01`,
  `c06`, `c09`-`c13`, the store behaviour a consuming repository runs against
  its own project.

## Repository surface and retained primitive gates

The requirement rows above are necessary but not sufficient. These additional
local CTest IDs prevent a complete-looking conformance suite from hiding deleted
regressions or forbidden compatibility surface. Stop condition 2 requires this
report to carry every local CTest ID, so the list below is the whole of
`ctest -N` that the 59 gates above do not already name. With them it is 83
registered tests, which is what a green run must report:

- CTEST `check-repository-surface`
- Four aggregates under the `CONFORMANCE` label, each running every compiled
  case in its binary including the ones no requirement ID names:
  CTEST `test-contract-operator`, CTEST `test-contract-runtime`,
  CTEST `conformance-umbraflow` and CTEST `conformance-arcana`. The last
  two were added 2026-08-10 with the exported Operator conformance suite (see
  [the next block](2026-08-10-next-block.md) §5); they run cases through the
  public entry point a consumer uses, so a change that keeps the in-tree
  fixtures green but breaks the exported surface is visible. The first two
  gained the same aggregate in `dcc43b5`, after seven compiled cases in
  `test-contract-operator` were found running under no CTest at all.
- CTEST `test-core`
- CTEST `test-domain`
- CTEST `test-engine`
- CTEST `test-controller`
- CTEST `test-script`
- CTEST `test-task`
- CTEST `test-trace`
- CTEST `test-cli`
- CTEST `test-vision`, CTEST `test-image` and CTEST `test-operator`
- CTEST `test-jcs-luau`, CTEST `test-runtime-model-v2-luau`,
  CTEST `test-resolution-v2-luau` and CTEST `test-receipt-request-v2-luau`,
  under the `LUAU` label. `tests/CMakeLists.txt` also fails configure when a
  `tests/task/*.luau` file is in none of them.
- CTEST `check-modules`, CTEST `check-safety`, CTEST `test-check-safety` and
  CTEST `test-annotate-backend`
- CTEST `test-ocr-real` is registered only on a host that has created
  `tests/assets/real-regression`, and carries the `REAL` label rather than
  `CI`. It is the one ID `ctest -N` may legitimately not list.

`check-repository-surface` is implemented by
`tests/test-runtime-surface.py` and registered directly with CTest. It fails
when a retired file/schema/CLI command reappears, a business global exports a
Runtime/Receipt/input capability, C++ contains RuntimeModel TOML field
interpretation, more than one trusted semantic parser is registered, a
consumer/game symbol enters generic schemas, or a Receipt authority field has
no Host-delivery validation case.

The retained regression binaries own these capabilities:

| Existing CTest | Required retained behavior |
|---|---|
| `test-core` | strong IDs, non-wrapping generation, checked arithmetic, monotonic time |
| `test-domain` | frame/space/hash value invariants |
| `test-engine` | capture/session/ObservationLease and target-generation primitives |
| `test-controller` | native input, DPI, capture and delivery-time revalidation primitives |
| `test-script` | VM allowlist, cancellation and resource ceilings |
| `test-task` | cycle ledger, TaskHost lifecycle, deterministic harness and privileged Explore isolation |
| `test-trace` | generic recorder/sink/event ordering and stream validation |
| `test-cli` | allowed command surface and privileged Explore startup only |

## Inherited implementation audit

| Area | Disposition | Required action |
|---|---|---|
| IDs, checked arithmetic, frame/space/hash, capture, cycle ledger, target revalidation | KEEP | preserve behavior; rerun existing regression and new attack tests |
| controller native input/DPI/window generation | KEEP | move final authorization behind Host.deliver; no second input caller |
| Script VM sandbox/cancellation/resource limits and TaskHost lifecycle | KEEP | restore removed coverage; remove runtime globals that bypass Operator |
| generic Trace recorder/sink/event ordering | KEEP | restore generic tests; do not restore obsolete runtime replay semantics |
| `entry/cli/check*`, `run*`, `replay*`, file-frame source | DELETE | remove declarations, dispatch, CMake registration, and tests tied to old semantics |
| v1/UFR/envelope schemas | DELETE | keep deleted; no reader, alias, optional fallback, or dual spelling |
| old `ctx/hits/navigation/mint/oracle/recognition/replay/regress` runtime semantics | DELETE | remove files and framework exports; rebuild only named v2 modules |
| current Runtime v2 schema/model/evidence/observe/resolution/project | REWRITE | implement RA/RM ownership, unique parser, UiTarget/Binding split, Host binding |
| current page-model interception and public manifest acceptance | REWRITE | exact root `runtime-model.toml`, confinement first, private finalize capability |
| current Receipt/click FFI | REWRITE | opaque Host ledger and one `Host.deliver`; no raw coordinate mint API |
| `tools/annotate/store.py` and `publication.py` | REWRITE | minimal AW SQLite, immutable release, replay gate, separate deployment activation |
| current reduced Host/Task/Trace tests | REWRITE | restore primitive assertions, then add exact contract IDs above |
| Operator/ProjectRegistration/session manifests | REWRITE | add minimal `modules/operator` and PR/OP/PL/JR schemas; no game fields |

Every inherited dirty path is closed by
[`2026-08-09-runtime-migration-disposition.manifest.json`](2026-08-09-runtime-migration-disposition.manifest.json):
101 unique paths, 67 `REWRITE`, 34 `DELETE`, no implicit/default
disposition. Its canonical disposition root is
`fdf9d26ff6bbcc2cd56c3c5c8f35ea9b488d78f24cdd8b2a5b5de8d9fc5aa09d`.
`KEEP` in the capability table refers to behavior retained from the clean
base, not acceptance of inherited dirty bytes; `ALREADY_SATISFIED` is empty
until a named gate proves it.

## Stop conditions

Implementation stops if:

1. the consumer bundle root changes;
2. a schema path or test ID changes without updating this report first;
3. any consumer-specific symbol enters generic C++/Luau/SQLite schemas;
4. any old reader, alias, fallback, direct action entry point, or screenshot
   dependency is retained for compatibility;
5. baseline and disposition manifests differ in path set/order, or any entry
   lacks exactly one explicit disposition.

The real dual-game gate is currently `NOT_RUN`: this upstream-only worktree
does not own two real consumer registrations and records no fabricated
`project_registration_hash`. That status blocks consumer production mutation,
not completion of the generic upstream framework.
