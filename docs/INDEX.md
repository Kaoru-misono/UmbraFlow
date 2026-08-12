# Documentation Index

Read the current contract first, then the one unfinished-work plan. Historical
plans explain how a decision was reached but do not open additional work.

## 1. Current contract

- [Runtime v2 and game-operator breaking authority](plans/2026-08-09-runtime-hardening-rewrite.md)
  — the frozen upstream implementation authority and consumer bundle pin.
- [`schema/umbraflow-runtime-v2.schema.json`](../schema/umbraflow-runtime-v2.schema.json)
  — normative runtime-model shape, with the
  [runtime model contract](plans/2026-08-09-runtime-model-contract.md) as prose.
- [Architecture](ARCHITECTURE.md) — module ownership and deliberate absences.
- [Domain glossary](../CONTEXT.md) — terminology authority.
- [Product form and roadmap](plans/2026-07-21-product-form-and-roadmap.md) —
  product direction, not current execution status.

The project-directory decision and its measurements are complete historical
records:
[project as data](archive/plans/2026-08-11-project-as-data.md) and
[its inventory](archive/plans/2026-08-11-project-as-data-inventory.md).

## 2. Current work

- The consumer repository's `docs/architecture/parallel-implementation-plan.md`
  is the only canonical list of unfinished work. It contains current blockers,
  upstream correctness debt, Runtime behaviour, plugin/schema work, read-only
  consumer obligations and documentation cleanup. Its predecessor is archived at
  [`docs/archive/plans/2026-08-12-outstanding-work.md`](archive/plans/2026-08-12-outstanding-work.md),
  which names where every row went.
- [Current execution checklist](TODO.md) retains the two release-gate boxes that
  are useful as checkboxes. It is not a second complete work inventory.
- [Plans index](plans/README.md) lists current authorities and every archived
  plan.

## 3. Current blockers

The exact acceptance criteria live in the consumer repository's parallel
implementation plan. In dependency order, the immediate blockers are:

1. Finalize the consumer's uncommitted v1.14 bundle and re-pin only committed,
   jointly reviewed bytes (`O-001`).
2. Make `linux-analysis` compile clean with a reported object denominator
   (`O-002`).
3. Run the W2-W7 adversarial round and obtain the missing G0 PASS verdicts
   (`O-003`).
4. Run the current head through remote CI (`O-004`).
5. Finish the retroactive `core` admission review (`O-005`).
6. Give `a03` and `a05` per-requirement behavioural gate IDs (`O-006`).
7. Replace delete-and-recreate Operator database handling before consumer C3
   (`O-007`).

The real dual-game attestation remains external and cannot be moved by fixtures.

## 4. Migration and evidence

- [Runtime migration report](plans/2026-08-09-runtime-migration-report.md) —
  inherited baseline, dispositions and requirement-to-gate map. Verify it
  against current paths before relying on an old locator.
- Operator databases are currently refused on exact DDL mismatch rather than
  migrated, and refusal leaves the file untouched. The exact stored DDL is the
  sole schema identity, ruled 2026-08-12 in
  [the execution checklist](TODO.md) under the delete-on-open deadline, which
  also states what a migration must name. The deadline for replacing that
  development-only behaviour is `O-007` in the consumer repository's parallel
  implementation plan.
- A consumer writes a project directory and runs
  `umbra-flow-conformance --project <directory>`; it compiles no Umbraflow C++.
- The checked-in upstream pin is v1.13. A dirty v1.14 draft exists in the
  read-only consumer worktree as of 2026-08-12, so the full bundle check is
  expected to refuse until `O-001` is completed.

## Before investigating a failure

Read [Pitfalls](pitfalls/README.md), beginning with
[checks that cannot fail](pitfalls/checks-that-cannot-fail.md). A green name is
not evidence unless its failure mode was observed.

## History

`archive/plans/` and `archive/reviews/` retain completed, superseded and
measurement-only documents. Each plan moved in the 2026-08-12 consolidation
states at its top whether nothing remains owed or which consolidated row owns
what survived.
