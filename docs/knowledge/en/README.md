# Architecture Knowledge Base

This directory records UmbraFlow's current executable architecture so developers can find module
responsibilities, key entry points, and tests. Module dependencies are governed by
[`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md); decided but unfinished work is tracked under
[`docs/plans/`](../../plans/README.md).

## Recommended reading order

1. [`00-overview.md`](00-overview.md) — Start with the system map and primary runtime path.
2. [`module-core.md`](module-core.md) — Errors, numeric safety, ownership, and foundational types.
3. [`module-domain.md`](module-domain.md) — Frame identity, coordinate spaces, target generations,
   detections, and observation leases.
4. [`module-vision-image.md`](module-vision-image.md) — Gray8/SAD recognition, PNG, pixel layouts,
   and template assets.
5. [`module-annotation.md`](module-annotation.md) — Authoring documents, runtime manifests, page
   resolution, and action authorization.
6. [`module-engine.md`](module-engine.md) — Published-artifact loading, same-frame decisions, port
   orchestration, action execution, and tracing.
7. [`module-script.md`](module-script.md) — The Luau substrate: sandbox, budgets, interrupt
   cancellation, and the framework/project environment split.
8. [`module-controller.md`](module-controller.md) — WGC, target continuity, DPI, and
   strict-background input.
9. [`entry-workbench.md`](entry-workbench.md) — GUI editing, capture, preview, compilation, and
   publication.
10. [`entry-cli.md`](entry-cli.md) — Argument parsing, offline loading, Windows adapters, and exit
    codes.
11. [`entry-m0-demo.md`](entry-m0-demo.md) — The frozen on-hardware acceptance program and its
    boundary with product code.

## Two pages still missing (2026-07-29)

`modules/task` and `modules/trace` have no page of their own, and since stage 3 of 2026-07-29 moved
task policy wholesale into those two layers, the engine and CLI pages have been explaining things
that are not theirs. The suggested scope is below. Write them once stage 3d (semantic events plus
the validation state machine) has landed, so they are not stale on arrival:

- **`module-task.md`** — `TaskHost`'s D10 verb shape and the run lifecycle; the capability surface's
  two seams (`installer()` for the data tables, `privateCapabilities()` for the primitive table) and
  why the second is private; the observation-cycle protocol and `CycleLedger`; the carrier shapes of
  Tier A/B/C; which policy the trusted Luau framework (`runtime/ctx.luau` plus `runtime/task.luau`)
  owns, and exactly where the "C++ owns guarantees, Luau owns policy" line falls. It must **not**
  restate the sandbox, budgets, and dual environments that `module-script.md` already covers.
- **`module-trace.md`** — schema ownership of `umbraflow-trace/v1`; the event families (`run.*`,
  `engine.*`, `task.native_call`, and `framework.*` after 3d); field ordering and the rules for
  golden comparison; the documented non-golden field set; `ITraceSink`'s synchronous fallible
  contract and the failure-precedence rules; and the "audit log, not a replay log" positioning.
