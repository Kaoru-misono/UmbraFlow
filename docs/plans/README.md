# Plans

The current authorities live here, and nothing else does. This is the canonical
plan location required by `CLAUDE.md` and `AGENTS.md`. Superseded and completed
plans are under [`docs/archive/plans/`](../archive/plans/); closed reviews are
under [`docs/archive/reviews/`](../archive/reviews/). Historical research there
must not override the authorities below.

## Current authority

Reading order: target form, then layer ownership, then the model semantics.

- [Runtime v2 and game-operator breaking rewrite](2026-08-09-runtime-hardening-rewrite.md) —
  frozen breaking authority for Annotation/Observation Runtime v2, Host Receipt/delivery ownership,
  Operator Core, and the multi-game ProjectPlugin boundary. Its pinned product design overrides every
  conflicting Page/Element/Hit/UFR, Context, direct-run, caller-measurement, caller-effect, and
  pre-Operator action clause below. No compatibility implementation is permitted.
- [Runtime v2 migration report](2026-08-09-runtime-migration-report.md) —
  reproducible inherited baseline, KEEP/REWRITE/DELETE dispositions, and the exact
  requirement-to-owner/schema/CTest map.
- [The next block after runtime hardening](2026-08-10-next-block.md) --
  proposed, nothing built on its account. Establishes that the tree is mid Phase
  2A rather than past it, that this branch has never been pushed and so has
  never been through CI, and orders the deferred work by dependency.

## Retained predecessor references

The documents below preserve prior decisions and measurements. They are
superseded wherever they conflict with the current authority above.

- [Three layers and the Agent operator](2026-08-01-three-layers-and-agent-operator.md) —
  developer-approved 2026-08-01 **target form**: the C++/framework/business split, the Agent
  as a third operator with its own front end, the run/explore trust modes, annotation as the
  inter-layer contract (four added rulings), the page / named-appearance / unnamed-appearance
  modelling trichotomy, and the full verb surface. Amends the three-layer plan below; does not
  overturn it.
- [Script-owned page model](2026-07-31-script-owned-page-model.md) — developer-ruled
  2026-07-31, reconciled with the target form in §十二, **migration in progress**. `element`
  and `page` move up to the trusted Luau framework; C++ keeps primitives only (`cycle_match`,
  `cycle_read`, `cycle_click`, project file I/O) plus the ticket ledger that makes acting on an
  observation safe. Amends the layer-one boundary of the three-layer plan and relocates the
  annotation model — its design conclusions stand, its implementation location does not. §九 is
  the retirement list; §十 holds the undecided page-graph shape that currently blocks layer two.
- [Product form and Roadmap](2026-07-21-product-form-and-roadmap.md) — product direction,
  P0–P3 scope and exit criteria.
- [Three-layer task system](2026-07-29-three-layer-task-system.md) — developer-approved
  2026-07-29 authority for the task system: C++ guarantee layer, bundled trusted Luau
  framework, and project-owned tasks; the observation-cycle protocol, the `uf` script root,
  the merged `umbraflow-trace/v2` stream, and the staged implementation (stages 1–3 landed).
  Its layer-one clauses (`cycle_page` / `cycle_find`, the `engine -> annotation` edge) are
  amended by the script-owned page model above.
- [Annotation model — capabilities, holding, appearances](2026-07-31-annotation-model-capabilities.md) —
  developer-approved 2026-07-31, slimmed 2026-08-01 to what stays live: §二 (the design) and
  §四之二 (seven added rulings), plus the open questions in §五. The capability set
  `{identify, interact, read}`, `PageReference` with `Holding` and exercised capabilities, named
  `Appearance`s and `ElementId` are all landed, under `umbraflow-authoring/v4`,
  `umbraflow-annotations/v3` and `umbraflow-trace/v2`. Its **model conclusions remain
  authoritative**; where that model is implemented is superseded by the script-owned page model
  above.
- [Agent front end and the exploration environment](2026-08-01-agent-front-end-and-exploration.md) —
  developer-authorised 2026-08-01, the implementation shape of the second Luau environment:
  which verbs are privileged, how the closure isolation is built, and the `annotation.*` trace
  vocabulary. Opens no ruling of its own; it pins the shape the target form above decided.
- [State layer and policy slots](2026-08-04-state-layer-and-policy-slots.md) —
  direction settled with five rulings answered 2026-08-04, **four phases none executed**. The
  `l2-v2` schema, `expected_presence`, the appearance gate, and the co-resolution matrix. Phase A
  landed the matrix on 2026-08-04; B depends on A's evidence, C on B, D on C plus the real machine.
- [Storing the evidence corpus](2026-08-04-evidence-storage.md) — four tiers for keeping
  screenshots out of version control. **Tier 4 shipped 2026-08-04** (`assets/screens` left git,
  `.git` 148 MB → 413 KB); tier 0 was retired by measurement; tiers 1–3 remain proposals.
- [Framework capabilities for full-map planning](2026-08-05-map-verbs-and-connectivity.md) —
  direction settled 2026-08-05, ordered and pending execution. Two new verbs (an atomic
  `drag(start, offset)`, a two-point connectivity read), one conditional item (kind enumeration,
  only if the expanded map page turns out not to sit on a regular grid), and a stitched-map
  evaluation separate from the screen matrix. It also rules three things OUT: a frame-difference
  primitive, general line-segment detection, and cross-cycle coordinate identity. The
  game-specific measuring and annotation is in `E:\umbraflow-projects\uf-chaos\MAP.md`.

## Proposals awaiting a decision

Nothing here is an authority. Each records measurement and a proposal; no part of
it has been approved, and no code has been changed on its account.

- [Luau coding standard — measurements and outline](2026-08-02-luau-coding-standard.md) —
  2026-08-02 survey of the 15 trusted framework modules across six dimensions, the
  outline of a standard to sit beside the C++ one, six questions deliberately left
  unruled, and 15 changes ranked by value. Three of them are defects rather than
  style, listed separately in [`docs/TODO.md`](../TODO.md). Every row is one
  approval decision; none are bundled.

## Retained reference

- [M0 demo port deviations](2026-07-20-m0-demo-port-deviations.md) — frozen real-machine
  acceptance reference pending parity and retirement.

A plan should be self-contained: it should include enough research, file paths,
implementation steps, and verification commands for another agent to execute it
without rediscovering context.

When a plan is completed, superseded, or verified, archive it with the archive
workflow rather than leaving it mixed into the authorities above.
