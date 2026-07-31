# Plans

Active implementation plans and retained authorities live here. This is the canonical plan
location required by `CLAUDE.md` and `AGENTS.md`.

## Current authorities

- [Product form and Roadmap](2026-07-21-product-form-and-roadmap.md) — product direction,
  P0–P3 scope and exit criteria.
- [Luau task-model grill decisions](2026-07-21-lua-task-model-grill-decisions.md) — implementation-level
  decisions to revalidate when entering each phase.
- [Script-owned page model](2026-07-31-script-owned-page-model.md) — developer-ruled 2026-07-31,
  **not yet implemented**. `element` and `page` move up to the trusted Luau framework; C++ keeps
  primitives only (a rect to pixels, a rect and a point to a click, a rect to OCR text) plus the
  ticket ledger that makes acting on an observation safe. Amends the layer-one boundary of the
  three-layer plan below and changes where the annotation model lives — its design conclusions
  stand, its implementation location does not.
- [Annotation model — capabilities, holding, appearances](2026-07-31-annotation-model-capabilities.md) —
  developer-approved 2026-07-31 authority for the annotation data model: the capability set
  `{identify, interact, read}`, `PageReference` with `Holding` and exercised capabilities, named
  `Appearance`s, `ElementId`, the `umbraflow-authoring/v4` and `umbraflow-annotations/v3`
  schema bumps, and the retirement of the workbench GUI. Supersedes the data-model clauses of the
  S0 contract below. Its **model conclusions remain authoritative**; where that model is
  implemented is superseded by the script-owned page model above.
- [P0-A Visual Annotation System & Data Model](2026-07-22-annotation-design.md) — developer-approved,
  S0-locked authority for template and search geometry, page resolution, action evidence, and the
  determinism/trace/strict-background constraints. Its **data model and schema** clauses, and the
  P0-A workbench GUI it specifies, are superseded by the 2026-07-31 plan above; see the amendment
  note at the top of the document.
- [Three-layer task system](2026-07-29-three-layer-task-system.md) — developer-approved
  2026-07-29 authority for the task system: C++ guarantee layer, bundled trusted Luau
  framework, and project-owned tasks; the observation-cycle protocol, the `uf` script
  root, the merged `umbraflow-trace/v2` stream, and the staged implementation. Supersedes
  the draft below in full and the script-layer rulings of
  [P0-B script layer](2026-07-27-p0b-script-layer.md).

Superseded, retained as history (do not implement from these):

- [Luau-first task system design draft](2026-07-28-luau-first-task-system-design-draft.md) —
  proposed three-layer split between the C++ driver kernel, the bundled trusted Luau framework,
  and project-owned game tasks; superseded in full on 2026-07-29 after
  [the 2026-07-28 review](../reviews/2026-07-28-luau-first-draft-review.md).
- [Workbench UI redesign](2026-07-27-workbench-ui-redesign.md) — superseded 2026-07-31: the GUI it
  redesigns was archived in `b57b67b`. Its observations about the authoring loop survive; nothing
  in its layout sections is actionable.
- [Page-centric authoring](2026-07-26-page-centric-authoring.md) — landed; its `shared`-flag ruling
  was reversed and its "runtime contract stays frozen" promise ended on 2026-07-31. See the
  redirect note at the top of the document.

Active implementation plans (not authorities, executed when entering the phase):

- [Engine architecture](2026-07-23-engine-architecture.md) — platform-free engine boundary and
  `umbra-flow run` composition.
- [Luau integration plan](2026-07-21-luau-integration-plan.md) — P0-B foundation: submodule +
  `modules/script` layout; steps 1–2 are complete and sandbox/cancellation work remains open.
- [P0-B Luau hardening ledger](2026-07-21-p0b-luau-hardening-ledger.md) — implementation-time
  checklist from two independent reviews of the Luau decision (cancel via lua_break, deep-freeze,
  determinism gaps); apply during P0-B.
- [Post-port Win32 robustness](2026-07-20-post-port-win32-robustness.md) — landed delivery-edge
  hardening plus open occlusion, capture cancellation, and timeout-pairing work.

Retained reference:

- [M0 demo port deviations](2026-07-20-m0-demo-port-deviations.md) — frozen real-machine
  acceptance reference pending parity and retirement.
- [Full-project review implementation follow-up](2026-07-28-full-project-review-fixes.md) —
  completed 2026-07-28 implementation record, including the explicit frozen-target deferrals.

Completed plans are under [`docs/archive/plans/`](../archive/plans/), and closed
reviews are under [`docs/archive/reviews/`](../archive/reviews/). Historical
research there must not override the current authorities above.

A plan should be self-contained: it should include enough research, file paths,
implementation steps, and verification commands for another agent to execute it
without rediscovering context.

When a plan is completed and verified, archive it with the archive workflow
rather than leaving completed work mixed into active planning.
