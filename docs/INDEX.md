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
- [Current execution checklist](TODO.md) retains useful release-gate checkboxes.
  It is not a second complete work inventory.
- [Plans index](plans/README.md) lists current authorities and every archived
  plan.

## 3. Current blockers

The exact dependency order and acceptance criteria live only in the consumer
repository's `docs/architecture/parallel-implementation-plan.md`. Do not copy
its changing status rows here. The 2026-08-13 documentation audits are
[indexed with the current plans](plans/README.md); they report completed review
evidence and point every surviving divergence back to that single execution
authority.

The real dual-game attestation remains external and cannot be moved by fixtures.

## 4. Migration and evidence

- [Framework capability survey](2026-08-14-framework-capability-survey.md) —
  measured implementation-symbol-to-test map; it is not an unfinished-work
  ledger.
- [Runtime migration report](plans/2026-08-09-runtime-migration-report.md) —
  inherited baseline, dispositions and requirement-to-gate map. Verify it
  against current paths before relying on an old locator.
- Operator database identity and mismatch handling are governed by the current
  implementation and the consumer execution authority. The 2026-08-12
  development-only delete-on-open description is historical; `O-007` landed on
  2026-08-13.
- A consumer writes a project directory and runs
  `umbra-flow-conformance --project <directory>`; it compiles no Umbraflow C++.
- The committed consumer bundle is v1.18 and pins five documents plus the
  bundle root. The implementation plan is deliberately outside the bundle.
  On 2026-08-13 the full check reported `SPEC BUNDLE: VERIFIED` at root
  `ac8c3fa652fb1601645d0c0bc04359bc75c9d08dc2883aa31ddeb94912f38ec4`.

## Before investigating a failure

Read [Pitfalls](pitfalls/README.md), beginning with
[checks that cannot fail](pitfalls/checks-that-cannot-fail.md). A green name is
not evidence unless its failure mode was observed.

## History

`archive/plans/` and `archive/reviews/` retain completed, superseded and
measurement-only documents. Each plan moved in the 2026-08-12 consolidation
states at its top whether nothing remains owed or which consolidated row owns
what survived.
