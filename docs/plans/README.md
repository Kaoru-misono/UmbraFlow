# Plans

This directory contains current authority. The one canonical list of
unfinished work lives in the consumer repository; see below. Completed,
superseded and measurement-only plans live under
[`docs/archive/plans/`](../archive/plans/); closed reviews live under
[`docs/archive/reviews/`](../archive/reviews/).

## Unfinished work

- The consumer repository's `docs/architecture/parallel-implementation-plan.md`
  is the only canonical owner of unfinished framework, runtime, plugin,
  consumer and documentation work. Older plans may explain a row, but they do
  not open a second copy of it. Its predecessor,
  [consolidated outstanding work](../archive/plans/2026-08-12-outstanding-work.md),
  is archived; its "Now lives" column names where every row went.

## Current authority

- [Runtime/game-operator breaking rewrite](2026-08-09-runtime-hardening-rewrite.md)
  — frozen implementation authority, and the ruling that removed the consumer
  bundle's exact-byte pin on 2026-08-16.
- [Runtime model contract](2026-08-09-runtime-model-contract.md) — field-level
  prose beside the normative
  [`umbraflow-runtime-v3.schema.json`](../../schema/umbraflow-runtime-v3.schema.json).
  Its body was rewritten against the current Runtime v3 schema on 2026-08-19.
- [Runtime migration report](2026-08-09-runtime-migration-report.md) — the
  requirement-to-owner/schema/CTest map, with its baseline and disposition
  manifests. It is an execution record, not an unfinished-work list.
- [Product form and roadmap](2026-07-21-product-form-and-roadmap.md) — product
  direction and milestone intent. Current execution status comes from the
  consumer execution authority, not its old phase prose.

## Archived on 2026-08-19

- [Upstream execution checklist](../archive/plans/2026-08-19-upstream-execution-checklist.md)
  — frozen without rewriting its final historical checkbox; consumer-plan
  `O-003` records the later two-region review PASS, and remaining release work
  stays in that consumer plan.
- [Three-layer task system](../archive/plans/2026-07-29-three-layer-task-system.md)
  and [three layers with the Agent operator](../archive/plans/2026-08-01-three-layers-and-agent-operator.md)
  — dated pre-v2 layer-design history; Runtime v3 and the live runtime contract
  own the current shape.
- [Carried-debt re-audit](../archive/reviews/2026-08-13-carried-debt-reaudit.md)
  — completed D-004 dispositions, with every survivor lifted to the consumer
  execution authority.
- [Cross-repository v1.18 audit](../archive/reviews/2026-08-13-cross-repository-audit.md)
  — completed D-005 evidence, superseding the v1.9 report.
- [Retroactive core admission review](../archive/reviews/2026-08-13-core-admission-review.md)
  — completed O-005 rulings for all fifteen imported files.
- [Framework capability survey](../archive/reviews/2026-08-14-framework-capability-survey.md)
  — measurement through 2026-08-17, superseded by Runtime v3 and later
  production wiring.
- [Runtime hardening review](../archive/reviews/2026-08-10-runtime-hardening-review.md)
  — every finding closed or accepted; consumer-plan `O-003` recorded the later
  final PASS rather than rewriting this earlier outcome record.

The two JSON files beside the migration report are its machine-readable
baseline and disposition data, not separate plans.

## Archived on 2026-08-15

- [Framework hash cleanup](../archive/plans/2026-08-14-framework-hash-cleanup.md)
  — all five stages implemented; the surviving consumer-side repair is owned by
  `CH-01a` in the consumer execution authority. The specification bundle pin it
  excluded was owned by the hash management proposal and **removed on
  2026-08-16** as that proposal's Stage 1; this plan's observation that
  `check-spec-bundle` really did carry the `CI` label is one of the three
  measurements that decided it.

## Archived on 2026-08-14

- [Script-owned page model](../archive/plans/2026-07-31-script-owned-page-model.md)
  — superseded by the runtime rewrite; its surviving confirmation/recognition
  behavior landed as `T-005` and is recorded in the live runtime model contract.

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
- [Consolidated outstanding work](../archive/plans/2026-08-12-outstanding-work.md)
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
