# Plans

This directory contains current authority and one canonical list of unfinished
work. Completed, superseded and measurement-only plans live under
[`docs/archive/plans/`](../archive/plans/); closed reviews live under
[`docs/archive/reviews/`](../archive/reviews/).

## Unfinished work

- [Consolidated outstanding work](2026-08-12-outstanding-work.md) — the only
  canonical owner of unfinished framework, runtime, plugin, consumer and
  documentation work. Older plans may explain a row, but they do not open a
  second copy of it.

## Current authority

- [Runtime v2 and game-operator breaking rewrite](2026-08-09-runtime-hardening-rewrite.md)
  — frozen implementation authority and the upstream copy of the consumer
  bundle pin.
- [Runtime model contract](2026-08-09-runtime-model-contract.md) — field-level
  prose beside the normative
  [`umbraflow-runtime-v2.schema.json`](../../schema/umbraflow-runtime-v2.schema.json).
  Its pre-v2 body is known documentation debt owned by `D-002` in the
  consolidated plan.
- [Runtime migration report](2026-08-09-runtime-migration-report.md) — the
  requirement-to-owner/schema/CTest map, with its baseline and disposition
  manifests. It is an execution record, not an unfinished-work list.
- [Product form and roadmap](2026-07-21-product-form-and-roadmap.md) — product
  direction and milestone intent. Current execution status comes from the
  consolidated plan, not its old phase prose.
- [Three-layer task system](2026-07-29-three-layer-task-system.md) and
  [three layers with the Agent operator](2026-08-01-three-layers-and-agent-operator.md)
  — retained layer and product-shape decisions, amended wherever Runtime v2
  overrides their old Page/Element vocabulary.
- [Script-owned page model](2026-07-31-script-owned-page-model.md) — superseded
  predecessor retained temporarily because source comments still cite it.
  Moving those citations and archiving the file is consolidated row `D-003`;
  its one surviving design question is `T-005`.

The two JSON files beside the migration report are its machine-readable
baseline and disposition data, not separate plans.

## Archived on 2026-08-12 in the consolidation pass

Each file below states at its top whether nothing remains owed or which row in
the consolidated plan owns what survived.

- [M0 demo port deviations](../archive/plans/2026-07-20-m0-demo-port-deviations.md)
- [Agent front end and exploration](../archive/plans/2026-08-01-agent-front-end-and-exploration.md)
- [Luau coding-standard survey](../archive/plans/2026-08-02-luau-coding-standard.md)
- [Evidence storage](../archive/plans/2026-08-04-evidence-storage.md)
- [State layer and policy slots](../archive/plans/2026-08-04-state-layer-and-policy-slots.md)
- [Map verbs and connectivity](../archive/plans/2026-08-05-map-verbs-and-connectivity.md)
- [Annotation-system work breakdown](../archive/plans/2026-08-09-annotation-agent-work-breakdown.md)
- [Annotation-v2 test matrix](../archive/plans/2026-08-09-annotation-v2-test-matrix.md)
- [Runtime/Operator handoff](../archive/plans/2026-08-09-claude-handoff.md)
- [Runtime annotation and Agent model](../archive/plans/2026-08-09-runtime-annotation-and-agent-model.md)
- [The next block](../archive/plans/2026-08-10-next-block.md)
- [Consumer attestation proposal](../archive/plans/2026-08-11-consumer-attestation.md)
- [Cross-repository drift audit](../archive/plans/2026-08-11-cross-repository-drift.md)
- [Project-as-data inventory](../archive/plans/2026-08-11-project-as-data-inventory.md)
- [Project as data](../archive/plans/2026-08-11-project-as-data.md)
- [Carried debt ledger](../archive/plans/2026-08-12-carried-debt-ledger.md)
- [Pure plugin helpers](../archive/plans/2026-08-12-plugin-pure-helpers.md)
- [Project content at scale](../archive/plans/2026-08-12-project-content-at-scale.md)
- [Recognition measurement](../archive/plans/2026-08-12-recognition-measured.md)

## Earlier archived plans

These left the live set before the 2026-08-12 consolidation. Their exact move
dates predate the canonical archive ledger; the Git history and each document's
closure note are the record.

- [Lua task-model grill](../archive/plans/2026-07-20-lua-task-model-grill.md)
- [Post-port Win32 robustness](../archive/plans/2026-07-20-post-port-win32-robustness.md)
- [Safe C++ core](../archive/plans/2026-07-20-safe-cpp-core.md)
- [UI verification runbook](../archive/plans/2026-07-20-ui-verification-runbook.md)
- [Task-model decision package](../archive/plans/2026-07-21-lua-task-model-decision-package.md)
  and [full transcript](../archive/plans/2026-07-21-lua-task-model-decision-package.FULL.md)
- [Task-model grill decisions](../archive/plans/2026-07-21-lua-task-model-grill-decisions.md)
- [Luau integration plan](../archive/plans/2026-07-21-luau-integration-plan.md)
- [P0-B hardening ledger](../archive/plans/2026-07-21-p0b-luau-hardening-ledger.md)
- [Locked annotation design](../archive/plans/2026-07-22-annotation-design.md)
- [Engine architecture](../archive/plans/2026-07-23-engine-architecture.md)
- [Page-centric authoring](../archive/plans/2026-07-26-page-centric-authoring.md)
- [P0-B script layer](../archive/plans/2026-07-27-p0b-script-layer.md)
- [Workbench UI redesign](../archive/plans/2026-07-27-workbench-ui-redesign.md)
- [Full-project review fixes](../archive/plans/2026-07-28-full-project-review-fixes.md)
- [Luau-first task-system draft](../archive/plans/2026-07-28-luau-first-task-system-design-draft.md)
- [Annotation capabilities model](../archive/plans/2026-07-31-annotation-model-capabilities.md)
- [Pre-rewrite worklist](../archive/plans/2026-08-05-worklist.md)
- [W2 EffectivePlan](../archive/plans/2026-08-10-w2-effective-plan.md)
- [W2-W7 reconciliation](../archive/plans/2026-08-10-w2-w7-reconciliation.md)
- [W3 Snapshot Coordinator](../archive/plans/2026-08-10-w3-snapshot-coordinator.md)
- [W4 delivery join](../archive/plans/2026-08-10-w4-delivery-join.md)
- [W6/W7 controller and Agent](../archive/plans/2026-08-10-w6-w7-controller-and-agent.md)
- [Consumer onboarding](../archive/plans/2026-08-11-consumer-onboarding.md)
- [Journal record binding](../archive/plans/2026-08-11-journal-record-binding.md)
- [TODO correction record](../archive/plans/2026-08-12-todo-correction-record.md)

## Archived reviews

- [Annotation backend review](../archive/reviews/2026-07-22-annotation-backend-review.md)
- [Repository simplification sweep](../archive/reviews/2026-07-25-simplify-sweep.md)
- [Full-project architecture review](../archive/reviews/2026-07-27-full-project-review.md)
- [Luau-first draft review](../archive/reviews/2026-07-28-luau-first-draft-review.md)
- [Third adversarial round](../archive/reviews/2026-08-10-third-round-review.md)

When a current authority is completed or superseded, move it normally into the
archive only after every surviving obligation has a row in the consolidated
plan and the archived file names that row.
