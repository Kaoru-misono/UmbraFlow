# Plans

Active implementation plans and retained authorities live here. This is the canonical plan
location required by `CLAUDE.md` and `AGENTS.md`.

## Current authorities

- [Product form and Roadmap](2026-07-21-product-form-and-roadmap.md) — product direction,
  P0–P3 scope and exit criteria.
- [Luau task-model grill decisions](2026-07-21-lua-task-model-grill-decisions.md) — implementation-level
  decisions to revalidate when entering each phase.
- [P0-A Visual Annotation System & Data Model](2026-07-22-annotation-design.md) — developer-approved,
  S0-locked authority for authoring/runtime schemas, template and search geometry,
  page resolution, action evidence, and the P0-A workbench.

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
