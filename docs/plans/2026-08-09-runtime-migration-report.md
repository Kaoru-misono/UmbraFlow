# Runtime v2 upstream migration report

Status: G0 execution map
Date: 2026-08-09
Scope: `umbraflow-cpp` only; no consumer-project writes

This report binds the v1.7 spec root
`90496173f8a920e59659b7c4568c8d04abe7b4d3d209db493a59fc3c55226ed0`
to the inherited upstream baseline rooted at
`55444b02a8ace9fe7493e5175618ef0a67d87402087874b7972187ac71ed8ac7`.
The executable conformance resolutions derived from the product bundle are
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

## Requirement ownership and test map

| ID | Owner | Schema location | Verification |
|---|---|---|---|
| P-01 | Operator facade | OP:`ToolInvocation/CommandRecord/Operation` | CTEST `contract-product-p01` |
| P-02 | Host + Operator | OP:`ControlTransition/ExternalInputFinding` | CTEST `contract-product-p02` |
| P-03 | Script sandbox | OP:`ControllerCapability` | CTEST `contract-product-p03` |
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
| S-01 | Snapshot coordinator | OP:`SnapshotParts` + JR:`ProjectState` | CTEST `contract-state-s01` |
| S-02 | Snapshot coordinator | OP:`ProjectSnapshot/SnapshotToken` | CTEST `contract-state-s02` |
| S-03 | Snapshot coordinator | OP:`SnapshotToken` | CTEST `contract-state-s03` |
| S-04 | Operator planner | OP:`DecisionBasis` | CTEST `contract-state-s04` |
| S-05 | Session coordinator | OP:`SessionManifest` + PR | CTEST `contract-state-s05` |
| S-06 | Project state store | JR:`ProjectInstance/ProjectState` | CTEST `contract-state-s06` |
| C-01 | Host control ledger | OP:`ControlLease/FencingToken` | CTEST `contract-control-c01` |
| C-02 | Host control ledger | OP:`SessionEpoch/ControlLease` | CTEST `contract-control-c02` |
| C-03 | Host delivery | OP:`DeliveryAuthority/ReceiptRef` | CTEST `contract-control-c03` |
| C-04 | Operator facade | OP:`ToolInvocation/CommandRecord` | CTEST `contract-control-c04` |
| C-05 | Operator planner | OP:`PlanProposal/EffectivePlan` + PR | CTEST `contract-control-c05` |
| C-06 | Operation ledger | OP:`CommandRecord/Operation` | CTEST `contract-control-c06` |
| C-07 | Operation ledger | OP:`OperationState/PlanVersion` | CTEST `contract-control-c07` |
| C-08 | Operator planner | OP:`EffectivePlan/UIActionIntent/WorkflowLimits` | CTEST `contract-control-c08` |
| C-09 | Operation ledger | OP:`DispatchOutcome/ToolResult` | CTEST `contract-control-c09` |
| C-10 | Operation ledger + Host | OP:`DispatchRecord/DeliveryOutcome` | CTEST `contract-control-c10` |
| C-11 | Reconciliation coordinator | OP:`ReconcileProposal` + JR | CTEST `contract-control-c11` |
| C-12 | Policy/approval ledger | PL + OP:`ApprovalToken/AuthorityDecision` | CTEST `contract-control-c12` |
| C-13 | Operation ledger | OP:`MutationChain` | CTEST `contract-control-c13` |
| C-14 | Operation ledger | OP:`DispatchRecord/OperationState` | CTEST `contract-control-c14` |
| A-01 | Agent event facade | OP:`SubscriptionCursor/ResyncRequired` | CTEST `contract-agent-a01` |
| A-02 | Agent runtime | OP:`AgentBudget/ProgressMarker` | CTEST `contract-agent-a02` |
| A-03 | Audit owners | TR + OP + JR + AW:`ReplayBundle` | CTEST `contract-agent-a03` |
| A-04 | Reconciliation coordinator | JR:`JournalEvent` | CTEST `contract-agent-a04` |
| A-05 | Publication gates | AW:`ReplayGate` + PR:`plugin_hash` | CTEST `contract-agent-a05` |
| A-06 | Deployment boundary | AW:`AuthoringCapabilityRoot` + RA | CTEST `contract-agent-a06` |
| A-07 | Host control ledger | OP:`ControlTransition/DeliveryAuthority` | CTEST `contract-agent-a07` |
| A-08 | Operator recovery | OP:`ExternalInputFinding/OperationState` | CTEST `contract-agent-a08` |

Planned test sources are:

- `tests/operator/test-product-contract.cpp` for `contract-product-*`;
- `tests/operator/test-project-plugin-contract.cpp` for
  `contract-product-p05-fixtures` and generic opaque-payload isolation;
- `tests/task/test-runtime-v2-contract.cpp` plus trusted Luau fixtures for
  `contract-runtime-*`;
- `tests/operator/test-state-contract.cpp` for `contract-state-*`;
- `tests/operator/test-control-contract.cpp` for `contract-control-*`;
- `tests/operator/test-agent-audit-contract.cpp` for `contract-agent-*`.

## Repository surface and retained primitive gates

The requirement rows above are necessary but not sufficient. These additional
local CTest IDs prevent a complete-looking contract suite from hiding deleted
regressions or forbidden compatibility surface:

- CTEST `contract-repository-surface`
- CTEST `test-core`
- CTEST `test-domain`
- CTEST `test-engine`
- CTEST `test-controller`
- CTEST `test-script`
- CTEST `test-task`
- CTEST `test-trace`
- CTEST `test-cli`

`contract-repository-surface` is implemented by
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
| current page-model interception and public manifest acceptance | REWRITE | exact root `page-model.toml`, confinement first, private finalize capability |
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
