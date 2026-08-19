# Documentation Index

Read the current contract first, then the one unfinished-work plan. Historical
plans explain how a decision was reached but do not open additional work.

## 1. Current contract

- [Runtime/game-operator breaking authority](plans/2026-08-09-runtime-hardening-rewrite.md)
  — the frozen upstream implementation authority and semantic contract-version
  ruling; its RuntimeModel field authority has since moved to v3.
- [`schema/umbraflow-runtime-v3.schema.json`](../schema/umbraflow-runtime-v3.schema.json)
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
- The [upstream execution checklist](archive/plans/2026-08-19-upstream-execution-checklist.md)
  is archived without rewriting its historical checkboxes; current status lives
  only in the consumer execution authority.
- [Plans index](plans/README.md) lists current authorities and every archived
  plan.

## 3. Architecture proposals

- [HostPlugin architecture proposal](archive/reviews/2026-08-14-host-plugin-architecture-proposal.md)
  — **ruled on and archived 2026-08-15.** Option A was approved, widened by a
  second identity defect, and is implemented; option B was refused because the
  evidence it was sold to purchase can be read today; option C was not
  considered. `ARCHITECTURE.md`'s deliberate-absence sentence stands unchanged.
  The one obligation that outlived it — hoist `modules/cli/source/cli/platform/`
  into a shared module when a second real assembly root exists — is owned by
  [the product roadmap](plans/2026-07-21-product-form-and-roadmap.md).
- [Hash management simplification proposal](2026-08-14-hash-management-simplification-proposal.md)
  — proposes zero developer-authored digests, removal of bundle/schema pins
  used as compatibility versions, and retention only of automatically produced
  aggregate content identity at real immutable-byte boundaries.

The HostPlugin document is a ruled historical review. The hash document remains
a proposal except for its explicitly approved and executed Stage 1. Neither is
current contract or a second unfinished-work list.

## 4. Current blockers

The exact dependency order and acceptance criteria live only in the consumer
repository's `docs/architecture/parallel-implementation-plan.md`. Do not copy
its changing status rows here. The completed 2026-08-13 documentation audits
are [archived and indexed](plans/README.md); they point every surviving
divergence back to that single execution authority.

The real dual-game attestation remains external and cannot be moved by fixtures.

## 5. Migration and evidence

- [Framework capability survey](archive/reviews/2026-08-14-framework-capability-survey.md)
  — archived implementation-symbol-to-test measurement through 2026-08-17;
  Runtime v3 and later production wiring supersede its locators.
- [Runtime migration report](plans/2026-08-09-runtime-migration-report.md) —
  inherited baseline, dispositions and requirement-to-gate map. Verify it
  against current paths before relying on an old locator.
- Operator database identity and mismatch handling are governed by the current
  implementation and the consumer execution authority. The 2026-08-12
  development-only delete-on-open description is historical; `O-007` landed on
  2026-08-13.
- A consumer writes a project directory and runs
  `umbra-flow-conformance --project <directory>`; it compiles no Umbraflow C++.
- The committed consumer bundle is at contract version v1.18 and holds five
  documents. The implementation plan is deliberately outside it.
  **The exact-byte root pin was removed on 2026-08-16**; the version is
  semantic, and the bytes that code consumes are pinned by the consumer's own
  `conformance/interface-lock/<version>/manifest.json` and checked by its
  `tests/contracts/test_interface_lock.py`. The ruling and the three
  measurements behind it are in the
  [rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md).

## Before investigating a failure

Read [Pitfalls](pitfalls/README.md), beginning with
[checks that cannot fail](pitfalls/checks-that-cannot-fail.md). A green name is
not evidence unless its failure mode was observed.

## History

`archive/plans/` and `archive/reviews/` retain completed, superseded and
measurement-only documents. Each plan moved in the 2026-08-12 consolidation
states at its top whether nothing remains owed or which consolidated row owns
what survived.
