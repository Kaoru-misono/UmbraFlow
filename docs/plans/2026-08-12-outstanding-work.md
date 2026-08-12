# Consolidated outstanding work

Status: **active and canonical for unfinished work**

Date: 2026-08-12

Scope: `umbraflow-cpp`, plus explicitly labelled read-only obligations in the
consumer repository. This file owns unfinished work only. Product authority
remains in [the runtime hardening rewrite](2026-08-09-runtime-hardening-rewrite.md),
field shape remains in the checked-in schemas, and terminology remains in
[`CONTEXT.md`](../../CONTEXT.md).

The plans archived on 2026-08-12 remain the evidence behind these rows. A row is
closed only by an implementation, a recorded owner ruling, or an explicit
supersession. Re-measuring an issue does not close it.

## 1. Current blockers

| ID | Work | Done when |
|---|---|---|
| `O-001` | **Finalize and re-pin the consumer specification bundle.** The read-only consumer worktree currently contains an uncommitted v1.14 draft while this repository pins v1.13. `python scripts/check_spec_bundle.py` reports every document digest and the bundle root different. Do not pin dirty bytes. | The consumer owner commits the bundle, its versioning decision is accepted, the four documents are reviewed together, both upstream pin copies move in one change, and the full bundle check passes. |
| `O-002` | **W11: make the analysis presets compile.** `linux-analysis` has never produced a clean, denominator-bearing result under `-Wunsafe-buffer-usage` and clang-tidy `WarningsAsErrors: '*'`. | A CI-faithful `linux-analysis` build reports no diagnostic and states how many objects were analysed. Run the Windows analysis preset as a second host check. |
| `O-003` | **W9 and the G0 PASS verdict.** No adversarial round covers all W2-W7 landings, and the two independent runtime-hardening reviews still end in FAIL even though their findings were closed or accepted. | The current range receives state/persistence and plugin-boundary re-review PASS verdicts, with every new finding closed or carried here. |
| `O-004` | **Run the current head through remote CI.** The remote branch exists, so the old wording that the branch was never pushed is false; the current local head is nevertheless ahead of it and has not been seen by CI. | The current head is pushed by the owner and all required remote jobs pass. |
| `O-005` | **W12: retroactive `core` admission review.** The four callerless facilities were evaluated and removed; the remaining fifteen files imported before `evaluate-core-capability` existed were not reviewed. | Every imported facility receives a recorded keep/move/delete ruling under the capability process. |
| `O-006` | **Give `a03` and `a05` independent requirement gate IDs.** Both behaviours exist behind aggregate coverage, but only `schema-agent-a03` and `schema-agent-a05` name them. | Each requirement has a falsifiable per-requirement behavioural gate, and the requirement map and CTest registration agree. |
| `O-007` | **Replace delete-and-recreate database handling before C3.** Today an Operator database with a different exact DDL fingerprint is refused and a developer deletes it manually. That is acceptable only before a real action is delivered on a user's behalf. | Before the first real C3 mutation, the owner rules and implements an audit-preserving upgrade/export/read/refusal path. |

## 2. Upstream correctness and contract debt

### 2.1 Tool, policy, and Agent authority

| ID | Work | Source debt |
|---|---|---|
| `O-101` | Restore or explicitly supersede the bundle's complete `ToolDescriptor`: required capabilities, effect bounds, UI-action bounds, per-tool workflow limits, timeout policy, mutability and idempotency. Remove the hard-coded global workflow ceiling if the bundle remains authoritative. | Cross-repository F-3/F-12.1; carried `C-R-1`. |
| `O-102` | Make policy an evaluated authority rather than a caller-supplied hash, and represent `required_approvals` as the ruled approver set rather than 0/1 derived from risk. | F-11; `C-R-2`; `ApprovalRequest::policyHash`. |
| `O-103` | Decide and implement the production join between control takeover and Host delivery. Production still has no owner that holds both an `OperatorCoordinator` and a `TaskHost`. | F-4 and the W4/a07 residue. |
| `O-104` | Complete the Agent event stream: delivery outcomes and Operation state changes must either reach `ledger_events` or the requirement and event vocabulary must be amended together. | `C-W67-1`, `C-W67-8`. |
| `O-105` | Implement the `p03` offer side. An Agent must receive a derived `available_tools` set that excludes Privileged tools; rechecking a presented invocation is only the accept side. | `C-W67-2`, F-12.3. |
| `O-106` | Make snapshot availability identity account for policy and the actual available-tool set. `event_cursor` now lands on `SnapshotRecord`; the remaining half is `available_tools` and its revision. | `C-W3-6`, `C-W3-8`, conditional `C-W3-9`, `C-W3-10`. |
| `O-107` | Rule whether a `SessionMode::Read` session may bind an Agent and whether no-progress carries a millisecond ceiling. Remove or produce/read `elapsed_without_progress_ms` accordingly. | `C-W67-3`, `C-W67-5`, `C-W67-6`. |

### 2.2 Ledger, snapshot, and delivery behaviour

| ID | Work | Source debt |
|---|---|---|
| `O-108` | Give `project_observations` and `ledger_events` bounded retention rules that preserve snapshot joins and make both `ResyncRequired` branches producible. | `C-W3-5`, `C-W67-4`. |
| `O-109` | Decide what `releaseLease` does with an outstanding dispatch: refuse release or resolve the unanswered outcome in the same transaction. | `C-W4-3`. |
| `O-110` | Decide whether the engine exposes a delivery-phase result so `beginDelivery`/coordinate-authorization failures can be `not_delivered` instead of the wider `transport_unknown`. | `C-W4-1`, requirement `c03`. |
| `O-111` | Decide whether one `TaskHost` is permanently bound to one controlled target or whether the fence and Receipt authority become per-target. | `C-W4-4`. |
| `O-112` | Re-check the three W4 assumptions that were prerequisites but were never recorded as run: multi-dispatch resolution count, W3 composition against SQLite serialization, and live UI composition through the Host. | `C-W4-8`. |
| `O-113` | Repair the two false snapshot claims: `identity_hash` is not invariant when `observation_id` moves, and the project-state join guards a conjunction rather than either clause independently. Account for the DDL fingerprint change caused by editing the embedded comment. | `C-W3-1`, `C-W3-2`, F-12.4. |
| `O-114` | Add a falsifier for `createSnapshot`'s single-mint/friend boundary. | `C-W3-11`. |
| `O-115` | Decide whether the exact Operator protocol schema gets a production reader and where an Operator is constructed in production. | `C-W2-1`. |

### 2.3 Schema, vocabulary, and hardening residue

| ID | Work | Source debt |
|---|---|---|
| `O-116` | Decide whether exact DDL fingerprint is the sole Operator schema identity; align `PRAGMA user_version`, fingerprint and upgrade policy. | `C-W4-2`. |
| `O-117` | Align the framework with the bundle on `ui_snapshot`, offline/online Agent terminology and `EffectEnvelope` versus `ExpectedEffect`; remove claims that attribute framework inventions to the bundle. | `C-W3-7`, `C-W67-11`, F-12.2/3/6/7. |
| `O-118` | Resolve `sessions.controller_kind`: publish it in the Operator schema or remove the DDL-only authority. | `C-R-3`. |
| `O-119` | Validate every Luau string before the explore JSON emitter calls `appendJsonString`; retain the deliberate non-JCS floating-point spelling with its rationale. | `C-R3-3`. |
| `O-120` | Make the member-initialization suppressions mechanically safe: add the missing construction assertions and narrow `NOLINTNEXTLINE` so a new member cannot be swallowed. | `C-R3-4`. |
| `O-121` | Close the known false-green tests and unexercised behaviour: W2 `T2/T10/T13`, W4 `T-10`, stored-but-unenforced workflow ceilings, `StepKind::Wait`, and the second-mechanism masking cases. | The archived next-block §2/§6 and carried W2/W4 rows. |
| `O-122` | Record or fix the second-`TaskContext` template-cache failure and the remaining test/process facts currently preserved only in archived landing notes. | `C-W4-5` to `C-W4-7`, documentation-only W2/W3/W67/R/R3 rows. |

## 3. Runtime behaviour and verification

| ID | Work | Done when |
|---|---|---|
| `T-001` | Resolve the same `interrupt` over two different base scenes and authorize its action. | A stable T03-equivalent gate exists. |
| `T-002` | Define collection/strip representation and ordered per-item geometry. | A T07-equivalent case returns count, stable indices and exact rectangles. |
| `T-003` | Store declared and observed transitions separately and report an observed destination that differs from policy without rewriting the model. | A T11-equivalent replay/offline gate exists. |
| `T-004` | Finish partial matrix coverage: real Reader/OCR unknown path (T05), one shared target reached from two different Surfaces (T06), and unmatched-visible-content diagnostics (T10). | Each partial row has a direct falsifiable test. |
| `T-005` | Rule the confirm-versus-recognise split: how one expected Surface is confirmed, when full resolution runs, and what follows failed confirmation. | The runtime API and failure policy have one owner and one testable contract. |
| `T-006` | Re-plan the approved map capability on Runtime v2: atomic `drag(start, offset)`, connectivity reading plus stitched-map evaluation, and conditional same-kind enumeration. | The old page-model assumptions are removed and each admitted verb has a runtime/conformance gate. |
| `T-007` | Decide the remaining map questions: wheel authorization, drag duration, colour-key ownership and the conditional enumeration falsifier. | Owner rulings are recorded here or in a successor plan. |
| `T-008` | Re-survey the current Luau runtime and decide executable Luau gates. The old survey's file and symbol inventory was deleted. | A current coding standard identifies reader-enforced rules, a falsifiable `check_luau` subset if useful, and whether `luau-analyze` is a required gate. |

## 4. Plugin environment and project schema work

| ID | Work | Done when |
|---|---|---|
| `P-001` | Publish the shared `umbraflow-fact-v1` fragment and compile project schemas with a closed referenced-document set that includes it. | The duplicated `$defs` block disappears without deleting a validation. |
| `P-002` | Decide whether `run.ended-v1` has no payload schema and make the manifest/loader express that honestly before deleting the file. | The event type is accepted without a fake schema, or the schema is retained with a reason. |
| `P-003` | Delete only schema keywords proven unreachable by the framework's evaluator and collapse the five-file payload-schema family after `P-001`. | Mutation cases show no live check was lost. |
| `P-004` | Add one negative document per cross-member conditional and run it through the consumer-facing conformance path. | Each conditional is observed refusing its own violation. |
| `P-005` | Consolidate the duplicated 4 MiB/16 MiB artifact ceilings and state their relationship to the VM memory quota once. | Registration and VM admission share one mechanically checked definition. |
| `P-006` | Add a several-hundred-KiB real consumer artifact case whose plugin reads and uses the value; prove it red when the record is missing or corrupted. | The conformance run, not only an in-tree unit fixture, exercises the path. |

`canon.encode` is not work owed. The host mints the surviving instance identity,
the decoded value boundary is live, the 23 globals have an exact test, the
environment material pins their names and published function contracts, and
the value/VM memory paths now have direct quota tests.

## 5. Consumer and real-machine work

These rows are read-only from this repository. They cannot be closed by an
upstream fixture.

| ID | Work | Done when |
|---|---|---|
| `C-001` | Author a correct Reader for the event page. The two-capture option-label annotation was removed and must not be patched back. | The annotation is justified against a representative corpus and produces normalized readings through the real Host. |
| `C-002` | Implement the real `chaos.dream.derive`: normalize Host readings, query the recognition projection, and return candidates, evidence and uncertainty. | Consumer C2 has a real producer rather than a fixture literal. |
| `C-003` | Create an honest production project registration only after the real plugin exists; the repository root correctly has none today. | Production loading accepts the root without publishing tools or deployments the product does not implement. |
| `C-004` | Rule the 334 `Conflict` encounters and the projection's degradation policy. | Every affected record is resolved, explicitly opaque, or excluded with auditable provenance. |
| `C-005` | Rule the consumer attestation container: one `attestations` root versus nine, and whether upstream ships a schema with no compiled reader. | Q1/Q2 have owner decisions. |
| `C-006` | Rule the first deterministic-build isolation level and the later cross-host requirement. | Q3 has an owner decision and a reproducible report shape. |
| `C-007` | Add a real consumer-side gate for `A-04`; prefer a conformance case over a self-attestation. | An unconfirmed fact is offered and the real plugin is observed not writing it as domain history. |
| `C-008` | Rule attestation predecessor semantics and correction of a false attestation. | Q5/Q6 have owner decisions and the immutable registration/session consequences are documented. |
| `C-009` | Produce and sign `D-01` through `D-09`, and add the background-only invariant to the consumer's evidence rather than relying on this repository's source scan. | The complete set is independently reviewed and bound into the consumer registration. |
| `C-010` | Complete consumer C3 without deleting an owed audit chain. | The first approved real mutation satisfies `O-007`, policy/approval, observation and Journal gates. |
| `C-011` | Complete C4 and the real dual-game attestation. | Two independently owned real consumers satisfy `attest-dual-game-p05`; fixtures do not count. |

## 6. Documentation and governance work

| ID | Work | Done when |
|---|---|---|
| `D-001` | Remove stale paths, counts and retired identifiers from current authorities: old consumer checkout, old worktree, `ProvidedProject`, deleted conformance layout, deleted v1 schemas and `FrontEnd`. | A repository-wide link/term sweep has no current-authority hit except an explicitly dated historical quotation. |
| `D-002` | Replace the pre-v2 body of the runtime model contract with prose matching `umbraflow-runtime-v2.schema.json`, while preserving the still-open behaviour contracts owned by `T-001` to `T-005`. | The document no longer names deleted schemas as authorities or teaches `Context`/`Target` as current records. |
| `D-003` | Move source comments that cite the superseded script-owned page-model plan onto current authority, then archive that predecessor. | No source file cites the live predecessor path and its confirm/recognise debt remains here as `T-005`. |
| `D-004` | Re-audit the carried debt against current line locations and close rows already changed, notably the landed `event_cursor` half of `C-W3-8`. | Every carried ID is closed, represented by an `O-*` row above, or explicitly retained as historical evidence only. |
| `D-005` | Re-run the cross-repository audit after v1.14 is committed. Determine whether co-versioning in v1.14 closes the old report's demand for a separate v2.0 bundle. | A current-version report replaces the v1.9 findings and every disagreement has one owner. |

## 7. Conditional proposals, not current blockers

| ID | Trigger | Proposal |
|---|---|---|
| `E-001` | A storage-format change is actually needed. | Hash decoded pixels rather than container bytes before changing screenshot encoding. |
| `E-002` | Local disk pressure justifies a decoder dependency. | Evaluate lossless WebP; screenshots are already outside Git, so repository size is not the trigger. |
| `E-003` | A page's annotation stabilizes and its matrix is green. | Evaluate retaining crops instead of full frames for that page, without deleting exploratory evidence globally. |

## 8. Verification order

For an upstream change, verify the narrow falsifier first, then the repository
checks, build and CI-labelled tests. For a bundle change, run the full
`scripts/check_spec_bundle.py`, not only the two-copy pin check. For consumer
work, record the exact consumer commit and keep real-game evidence distinct from
fixture evidence.

This plan is closed only when every unconditional `O-*`, `T-*`, `P-*`, `C-*`
and `D-*` row is closed or superseded by an owner ruling. `E-*` rows close when
their trigger is ruled unnecessary or when their measured implementation lands.
