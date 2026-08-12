# Consolidated outstanding work

Status: **superseded 2026-08-12. Not authoritative for any row below.**

The execution authority for every unfinished item in `umbraflow-cpp` and in the
consumer repository is now the consumer repository's parallel implementation
plan:

```text
uf-chaos: docs/architecture/parallel-implementation-plan.md
```

on this machine, `E:\umbraflow-projects\uf-chaos\docs\architecture\parallel-implementation-plan.md`.

Nothing in this file is still owed *here*. Every row was carried into that plan,
and the "Now lives" column of each table below names exactly where. A row is
either **absorbed** — its completion condition became part of a work package's
acceptance criteria, with the load-bearing wording carried across verbatim — or
**retained**, meaning it kept its identifier and became a first-class row of the
plan. No row was declared superseded: the direction changed, but no row's
subject became obsolete.

The rows are kept, unedited, so that an older reference lands somewhere that
says where the work went. Do not work from the text below; work from the plan.
If the two disagree, the plan is right and this file is stale by construction.

Date: 2026-08-12

The plans archived on 2026-08-12 remain the evidence behind these rows.

The plan's workflow series are `U1`–`U13` (upstream `umbraflow-cpp`),
`CH-01`–`CH-08` (consumer `uf-chaos`), `F0`–`F3` (Wave 0), and
`Q1`/`Q2`/`V1`/`G1`/`R1` (independent verification and second game). The
consumer series was `P1`–`P8` before 2026-08-12; it was renamed to `CH-01`–`CH-08`
because it collided with the `P-001`–`P-006` rows below once punctuation is
normalized. There is no alias: `P3` no longer names anything.

## 1. Current blockers

| ID | Work | Done when | Now lives |
|---|---|---|---|
| `O-001` | **Finalize and re-pin the consumer specification bundle.** The read-only consumer worktree currently contains an uncommitted v1.14 draft while this repository pins v1.13. `python scripts/check_spec_bundle.py` reports every document digest and the bundle root different. Do not pin dirty bytes. | The consumer owner commits the bundle, its versioning decision is accepted, the four documents are reviewed together, both upstream pin copies move in one change, and the full bundle check passes. | Retained as `O-001` in plan §2.1, and named as a gate in §7. Plan §2 corrects `F0` to "未完成：只在消费者一侧冻结" and records the measurement. The premise above is stale: the consumer bundle is committed at v1.16, not an uncommitted v1.14 draft. |
| `O-002` | **W11: make the analysis presets compile.** `linux-analysis` has never produced a clean, denominator-bearing result under `-Wunsafe-buffer-usage` and clang-tidy `WarningsAsErrors: '*'`. | A CI-faithful `linux-analysis` build reports no diagnostic and states how many objects were analysed. Run the Windows analysis preset as a second host check. | Retained as `O-002` in plan §2.1; gate in §7. |
| `O-003` | **W9 and the G0 PASS verdict.** No adversarial round covers all W2-W7 landings, and the two independent runtime-hardening reviews still end in FAIL even though their findings were closed or accepted. | The current range receives state/persistence and plugin-boundary re-review PASS verdicts, with every new finding closed or carried here. | Retained as `O-003` in plan §2.1; gate in §7. "carried here" now means carried into the plan. |
| `O-004` | **Run the current head through remote CI.** The remote branch exists, so the old wording that the branch was never pushed is false; the current local head is nevertheless ahead of it and has not been seen by CI. | The current head is pushed by the owner and all required remote jobs pass. | Retained as `O-004` in plan §2.1; gate in §7. The "ahead of it" half is now false too — `origin/design/annotation-system-v2` and `HEAD` are both `de8eb52`; the branch still has no CI run at all. Plan §2.1 records this. |
| `O-005` | **W12: retroactive `core` admission review.** The four callerless facilities were evaluated and removed; the remaining fifteen files imported before `evaluate-core-capability` existed were not reviewed. | Every imported facility receives a recorded keep/move/delete ruling under the capability process. | Retained as `O-005` in plan §9 (治理与文档工作). |
| `O-006` | **Give `a03` and `a05` independent requirement gate IDs.** Both behaviours exist behind aggregate coverage, but only `schema-agent-a03` and `schema-agent-a05` name them. | Each requirement has a falsifiable per-requirement behavioural gate, and the requirement map and CTest registration agree. | Absorbed into plan §3 `U12`（假绿测试、未执行断言与门禁）. |
| `O-007` | **Replace delete-and-recreate database handling before C3.** Today an Operator database with a different exact DDL fingerprint is refused and a developer deletes it manually. That is acceptable only before a real action is delivered on a user's behalf. | Before the first real C3 mutation, the owner rules and implements an audit-preserving upgrade/export/read/refusal path. | Retained as `O-007` in plan §2.1; `CH-08` depends on it explicitly (plan §4 `CH-08` and §7), because `CH-08` *is* the first real C3 mutation. `O-116` is recorded as its input. |

## 2. Upstream correctness and contract debt

### 2.1 Tool, policy, and Agent authority

| ID | Work | Source debt | Now lives |
|---|---|---|---|
| `O-101` | Restore or explicitly supersede the bundle's complete `ToolDescriptor`: required capabilities, effect bounds, UI-action bounds, per-tool workflow limits, timeout policy, mutability and idempotency. Remove the hard-coded global workflow ceiling if the bundle remains authoritative. | Cross-repository F-3/F-12.1; carried `C-R-1`. | Absorbed into plan §3 `U8`（ToolDescriptor、策略与授权）. |
| `O-102` | Make policy an evaluated authority rather than a caller-supplied hash, and represent `required_approvals` as the ruled approver set rather than 0/1 derived from risk. | F-11; `C-R-2`; `ApprovalRequest::policyHash`. | Absorbed into plan §3 `U8`. |
| `O-103` | Decide and implement the production join between control takeover and Host delivery. Production still has no owner that holds both an `OperatorCoordinator` and a `TaskHost`. | F-4 and the W4/a07 residue. | Absorbed into plan §3 `U10`（控制接管与 Host 投递的生产接合）. |
| `O-104` | Complete the Agent event stream: delivery outcomes and Operation state changes must either reach `ledger_events` or the requirement and event vocabulary must be amended together. | `C-W67-1`, `C-W67-8`. | Absorbed into plan §3 `U9`（Agent 会话、事件流与快照可用性）. |
| `O-105` | Implement the `p03` offer side. An Agent must receive a derived `available_tools` set that excludes Privileged tools; rechecking a presented invocation is only the accept side. | `C-W67-2`, F-12.3. | Absorbed into plan §3 `U8`; `CH-07` states it consumes U8's offer side rather than building a second one. |
| `O-106` | Make snapshot availability identity account for policy and the actual available-tool set. `event_cursor` now lands on `SnapshotRecord`; the remaining half is `available_tools` and its revision. | `C-W3-6`, `C-W3-8`, conditional `C-W3-9`, `C-W3-10`. | Absorbed into plan §3 `U9`. |
| `O-107` | Rule whether a `SessionMode::Read` session may bind an Agent and whether no-progress carries a millisecond ceiling. Remove or produce/read `elapsed_without_progress_ms` accordingly. | `C-W67-3`, `C-W67-5`, `C-W67-6`. | Absorbed into plan §3 `U9`. |

### 2.2 Ledger, snapshot, and delivery behaviour

The plan previously let `U2` claim exclusive semantic ownership of `ledger.cpp`
while none of these eight decisions had an owner. Owning the file is not a
disposition of the decisions inside it; plan §1 principle 9 now says so, and
`U2`'s acceptance criteria carry the ones that are ledger semantics.

| ID | Work | Source debt | Now lives |
|---|---|---|---|
| `O-108` | Give `project_observations` and `ledger_events` bounded retention rules that preserve snapshot joins and make both `ResyncRequired` branches producible. | `C-W3-5`, `C-W67-4`. | Absorbed into plan §3 `U2` acceptance criteria. |
| `O-109` | Decide what `releaseLease` does with an outstanding dispatch: refuse release or resolve the unanswered outcome in the same transaction. | `C-W4-3`. | Absorbed into plan §3 `U2` acceptance criteria. |
| `O-110` | Decide whether the engine exposes a delivery-phase result so `beginDelivery`/coordinate-authorization failures can be `not_delivered` instead of the wider `transport_unknown`. | `C-W4-1`, requirement `c03`. | Absorbed into plan §3 `U2` acceptance criteria. |
| `O-111` | Decide whether one `TaskHost` is permanently bound to one controlled target or whether the fence and Receipt authority become per-target. | `C-W4-4`. | Absorbed into plan §3 `U10`. |
| `O-112` | Re-check the three W4 assumptions that were prerequisites but were never recorded as run: multi-dispatch resolution count, W3 composition against SQLite serialization, and live UI composition through the Host. | `C-W4-8`. | Absorbed into plan §3 `U12`; the plan states it runs independently of `U2`. |
| `O-113` | Repair the two false snapshot claims: `identity_hash` is not invariant when `observation_id` moves, and the project-state join guards a conjunction rather than either clause independently. Account for the DDL fingerprint change caused by editing the embedded comment. | `C-W3-1`, `C-W3-2`, F-12.4. | Absorbed into plan §3 `U2` acceptance criteria, whole row: both false claims and the DDL fingerprint accounting live in the Operator DDL and its embedded comment. |
| `O-114` | Add a falsifier for `createSnapshot`'s single-mint/friend boundary. | `C-W3-11`. | Absorbed into plan §3 `U12`; independent of `U2`. |
| `O-115` | Decide whether the exact Operator protocol schema gets a production reader and where an Operator is constructed in production. | `C-W2-1`. | Absorbed into plan §3 `U3`, which already owns "所有路径经过 Coordinator". |

### 2.3 Schema, vocabulary, and hardening residue

| ID | Work | Source debt | Now lives |
|---|---|---|---|
| `O-116` | Decide whether exact DDL fingerprint is the sole Operator schema identity; align `PRAGMA user_version`, fingerprint and upgrade policy. | `C-W4-2`. | Absorbed into plan §3 `U2` acceptance criteria, and named there as the input `O-007` needs before an audit-preserving upgrade path can be defined. |
| `O-117` | Align the framework with the bundle on `ui_snapshot`, offline/online Agent terminology and `EffectEnvelope` versus `ExpectedEffect`; remove claims that attribute framework inventions to the bundle. | `C-W3-7`, `C-W67-11`, F-12.2/3/6/7. | Absorbed into plan §3 `U11`（插件环境、artifact 配额与词汇一致性）. |
| `O-118` | Resolve `sessions.controller_kind`: publish it in the Operator schema or remove the DDL-only authority. | `C-R-3`. | Absorbed into plan §3 `U11`, with the DDL-side change routed through `U2`. |
| `O-119` | Validate every Luau string before the explore JSON emitter calls `appendJsonString`; retain the deliberate non-JCS floating-point spelling with its rationale. | `C-R3-3`. | Absorbed into plan §3 `U11`. |
| `O-120` | Make the member-initialization suppressions mechanically safe: add the missing construction assertions and narrow `NOLINTNEXTLINE` so a new member cannot be swallowed. | `C-R3-4`. | Absorbed into plan §3 `U11`. |
| `O-121` | Close the known false-green tests and unexercised behaviour: W2 `T2/T10/T13`, W4 `T-10`, stored-but-unenforced workflow ceilings, `StepKind::Wait`, and the second-mechanism masking cases. | The archived next-block §2/§6 and carried W2/W4 rows. | Absorbed into plan §3 `U12`. |
| `O-122` | Record or fix the second-`TaskContext` template-cache failure and the remaining test/process facts currently preserved only in archived landing notes. | `C-W4-5` to `C-W4-7`, documentation-only W2/W3/W67/R/R3 rows. | Absorbed into plan §3 `U12`. |

## 3. Runtime behaviour and verification

No package in the plan owned these. They are retained with their identifiers as
the row table of the new upstream workflow `U13`（运行时行为模型 v2）, except
`T-008`, which is gate tooling. The plan states as a hard constraint on `CH-02`
and `CH-04` that neither may declare the runtime model complete while
`T-001`–`T-007` are open.

| ID | Work | Done when | Now lives |
|---|---|---|---|
| `T-001` | Resolve the same `interrupt` over two different base scenes and authorize its action. | A stable T03-equivalent gate exists. | Retained as `T-001` in plan §3 `U13`. |
| `T-002` | Define collection/strip representation and ordered per-item geometry. | A T07-equivalent case returns count, stable indices and exact rectangles. | Retained as `T-002` in plan §3 `U13`. |
| `T-003` | Store declared and observed transitions separately and report an observed destination that differs from policy without rewriting the model. | A T11-equivalent replay/offline gate exists. | Retained as `T-003` in plan §3 `U13`. |
| `T-004` | Finish partial matrix coverage: real Reader/OCR unknown path (T05), one shared target reached from two different Surfaces (T06), and unmatched-visible-content diagnostics (T10). | Each partial row has a direct falsifiable test. | Retained as `T-004` in plan §3 `U13`. |
| `T-005` | Rule the confirm-versus-recognise split: how one expected Surface is confirmed, when full resolution runs, and what follows failed confirmation. | The runtime API and failure policy have one owner and one testable contract. | Retained as `T-005` in plan §3 `U13`; `D-002`/`D-003` in plan §9 point at it. |
| `T-006` | Re-plan the approved map capability on Runtime v2: atomic `drag(start, offset)`, connectivity reading plus stitched-map evaluation, and conditional same-kind enumeration. | The old page-model assumptions are removed and each admitted verb has a runtime/conformance gate. | Retained as `T-006` in plan §3 `U13`. |
| `T-007` | Decide the remaining map questions: wheel authorization, drag duration, colour-key ownership and the conditional enumeration falsifier. | Owner rulings are recorded here or in a successor plan. | Retained as `T-007` in plan §3 `U13`. "here or in a successor plan" now resolves to the plan. |
| `T-008` | Re-survey the current Luau runtime and decide executable Luau gates. The old survey's file and symbol inventory was deleted. | A current coding standard identifies reader-enforced rules, a falsifiable `check_luau` subset if useful, and whether `luau-analyze` is a required gate. | Absorbed into plan §3 `U12`. |

## 4. Plugin environment and project schema work

| ID | Work | Done when | Now lives |
|---|---|---|---|
| `P-001` | Publish the shared `umbraflow-fact-v1` fragment and compile project schemas with a closed referenced-document set that includes it. | The duplicated `$defs` block disappears without deleting a validation. | Absorbed into plan §3 `U1`. `U1` and this row were the same work with two owners; the plan names `U1` as the single owner and forbids a second project-side publication. |
| `P-002` | Decide whether `run.ended-v1` has no payload schema and make the manifest/loader express that honestly before deleting the file. | The event type is accepted without a fake schema, or the schema is retained with a reason. | Absorbed into plan §3 `U1`. |
| `P-003` | Delete only schema keywords proven unreachable by the framework's evaluator and collapse the five-file payload-schema family after `P-001`. | Mutation cases show no live check was lost. | Absorbed into plan §3 `U1`. |
| `P-004` | Add one negative document per cross-member conditional and run it through the consumer-facing conformance path. | Each conditional is observed refusing its own violation. | Absorbed into plan §5 `Q1`（两仓库契约/golden/parity suite）. |
| `P-005` | Consolidate the duplicated 4 MiB/16 MiB artifact ceilings and state their relationship to the VM memory quota once. | Registration and VM admission share one mechanically checked definition. | Absorbed into plan §3 `U11`. |
| `P-006` | Add a several-hundred-KiB real consumer artifact case whose plugin reads and uses the value; prove it red when the record is missing or corrupted. | The conformance run, not only an in-tree unit fixture, exercises the path. | Absorbed into plan §3 `U11`. |

`canon.encode` is not work owed. The host mints the surviving instance identity,
the decoded value boundary is live, the 23 globals have an exact test, the
environment material pins their names and published function contracts, and
the value/VM memory paths now have direct quota tests. This paragraph moved to
plan §9.

## 5. Consumer and real-machine work

| ID | Work | Done when | Now lives |
|---|---|---|---|
| `C-001` | Author a correct Reader for the event page. The two-capture option-label annotation was removed and must not be patched back. | The annotation is justified against a representative corpus and produces normalized readings through the real Host. | Absorbed into plan §4 `CH-02` (production side) and `CH-04` (consumption side). Both conditions — 针对代表性 corpus 论证, 通过真实 Host 产出归一化读数 — are carried into `CH-02`'s acceptance criteria, and `CH-02` states that a package delivering only "a Reader" without them is weaker than this row and does not count. |
| `C-002` | Implement the real `chaos.dream.derive`: normalize Host readings, query the recognition projection, and return candidates, evidence and uncertainty. | Consumer C2 has a real producer rather than a fixture literal. | Absorbed into plan §4 `CH-05`, with the "真实生产者，而不是 fixture 字面量" condition carried verbatim; `CH-04` supplies the recognition projection it queries. |
| `C-003` | Create an honest production project registration only after the real plugin exists; the repository root correctly has none today. | Production loading accepts the root without publishing tools or deployments the product does not implement. | Absorbed into plan §4 `CH-06`, including the "只在真实插件存在之后" precondition. The "root has none today" claim was re-checked and still holds. |
| `C-004` | Rule the 334 `Conflict` encounters and the projection's degradation policy. | Every affected record is resolved, explicitly opaque, or excluded with auditable provenance. | Absorbed into plan §4 `CH-04`. |
| `C-005` | Rule the consumer attestation container: one `attestations` root versus nine, and whether upstream ships a schema with no compiled reader. | Q1/Q2 have owner decisions. | Absorbed into plan §5 `V1`. The `Q1`/`Q2` in the condition are the archived consumer-attestation plan's open questions, not the plan's `Q1`/`Q2` verification rows; the plan says so where it carries the row. |
| `C-006` | Rule the first deterministic-build isolation level and the later cross-host requirement. | Q3 has an owner decision and a reproducible report shape. | Absorbed into plan §4 `CH-01`, which already owns deterministic rebuild. `Q3` is the archived attestation plan's question. |
| `C-007` | Add a real consumer-side gate for `A-04`; prefer a conformance case over a self-attestation. | An unconfirmed fact is offered and the real plugin is observed not writing it as domain history. | Absorbed into plan §4 `CH-05`; needs the real plugin, so it cannot precede it. |
| `C-008` | Rule attestation predecessor semantics and correction of a false attestation. | Q5/Q6 have owner decisions and the immutable registration/session consequences are documented. | Absorbed into plan §5 `V1`. `Q5`/`Q6` are the archived attestation plan's questions. |
| `C-009` | Produce and sign `D-01` through `D-09`, and add the background-only invariant to the consumer's evidence rather than relying on this repository's source scan. | The complete set is independently reviewed and bound into the consumer registration. | Absorbed into plan §5 `V1`. |
| `C-010` | Complete consumer C3 without deleting an owed audit chain. | The first approved real mutation satisfies `O-007`, policy/approval, observation and Journal gates. | Absorbed into plan §4 `CH-08`, which is the first real C3 mutation and now depends on `O-007` explicitly. |
| `C-011` | Complete C4 and the real dual-game attestation. | Two independently owned real consumers satisfy `attest-dual-game-p05`; fixtures do not count. | Absorbed into plan §5 `G1`（第二真实游戏）. |

## 6. Documentation and governance work

| ID | Work | Done when | Now lives |
|---|---|---|---|
| `D-001` | Remove stale paths, counts and retired identifiers from current authorities: old consumer checkout, old worktree, `ProvidedProject`, deleted conformance layout, deleted v1 schemas and `FrontEnd`. | A repository-wide link/term sweep has no current-authority hit except an explicitly dated historical quotation. | Retained as `D-001` in plan §9. |
| `D-002` | Replace the pre-v2 body of the runtime model contract with prose matching `umbraflow-runtime-v2.schema.json`, while preserving the still-open behaviour contracts owned by `T-001` to `T-005`. | The document no longer names deleted schemas as authorities or teaches `Context`/`Target` as current records. | Retained as `D-002` in plan §9; `T-001`–`T-005` now sit in `U13`. |
| `D-003` | Move source comments that cite the superseded script-owned page-model plan onto current authority, then archive that predecessor. | No source file cites the live predecessor path and its confirm/recognise debt remains here as `T-005`. | Retained as `D-003` in plan §9; the confirm/recognise debt is `T-005` in `U13`. |
| `D-004` | Re-audit the carried debt against current line locations and close rows already changed, notably the landed `event_cursor` half of `C-W3-8`. | Every carried ID is closed, represented by an `O-*` row above, or explicitly retained as historical evidence only. | Retained as `D-004` in plan §9; "an `O-*` row above" now reads as a work package or row of the plan. |
| `D-005` | Re-run the cross-repository audit after v1.14 is committed. Determine whether co-versioning in v1.14 closes the old report's demand for a separate v2.0 bundle. | A current-version report replaces the v1.9 findings and every disagreement has one owner. | Retained as `D-005` in plan §9. The trigger has fired: the committed bundle is v1.16, not v1.14. The plan records the measurement without rewriting the original criterion. |

## 7. Conditional proposals, not current blockers

| ID | Trigger | Proposal | Now lives |
|---|---|---|---|
| `E-001` | A storage-format change is actually needed. | Hash decoded pixels rather than container bytes before changing screenshot encoding. | Retained as `E-001` in plan §10. |
| `E-002` | Local disk pressure justifies a decoder dependency. | Evaluate lossless WebP; screenshots are already outside Git, so repository size is not the trigger. | Retained as `E-002` in plan §10. |
| `E-003` | A page's annotation stabilizes and its matrix is green. | Evaluate retaining crops instead of full frames for that page, without deleting exploratory evidence globally. | Retained as `E-003` in plan §10. |

## 8. Verification order

Moved to plan §11. For an upstream change, verify the narrow falsifier first,
then the repository checks, build and CI-labelled tests. For a bundle change,
run the full `scripts/check_spec_bundle.py`, not only the two-copy pin check —
`--pins-only`, which is how CTest registers it, never opens the consumer
repository. For consumer work, record the exact consumer commit and keep
real-game evidence distinct from fixture evidence.

## 9. Rows that were already false when this file was superseded

Recorded here because a reader arriving from an old reference will otherwise
trust them. None of these makes its row closed; each is a stale premise inside
an open row, and the plan states the corrected measurement.

- `O-001` says the consumer bundle is an uncommitted v1.14 draft. It is
  committed (`63a4495`) at `bundle_version` 1.16, and its six document digests
  match the bytes on disk. What remains true is that this repository still pins
  v1.13, that all four pinned document digests and the bundle root differ, that
  two documents in the consumer manifest are not pinned here at all, and that
  the CTest gate runs `--pins-only` and therefore never reads the bundle.
- `O-004` says the local head is ahead of the remote branch. It is not:
  `HEAD` and `origin/design/annotation-system-v2` are both `de8eb52`. The rest
  of the row stands — that branch has no CI run at all.
- Plan `F0` was marked 完成 against a v1.14 baseline and commit `a1ec111`. The
  plan now marks it 未完成 and states which side is frozen.
- `D-005`'s trigger text waits for v1.14 to be committed; v1.16 is committed.
