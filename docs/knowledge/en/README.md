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
that are not theirs.

> **Timing updated (2026-07-29, `1fb41a7`)**: this used to say "write them once stage 3d has
> landed". **Stage 3 is now complete in full** (3d `4030ffd` semantic events plus the validation
> state machine; 3e/3f `1fb41a7` the framework unit tests and veto 6), so that condition is met.
> Write them **now, rather than waiting for stage 4's on-hardware run**: finishing stage 3 is what
> made these two layers' surface stable — all twelve primitives present, `umbraflow-trace/v1`'s
> event families fixed, the validation state machine landed in
> `modules/trace/source/trace/stream-validator.{hpp,cpp}`. Stage 4 *uses* that surface to write the
> first real daily and calibrate constants; it changes numbers, not shapes. Waiting for it only
> guarantees the pages are missing at the exact moment they are most needed — reading an
> on-hardware failure off a trace.

> **Count corrected (2026-07-30, `ed38124`)**: the private surface now holds **thirteen** primitives,
> not twelve — `key` was added beside `cycle_click`. The paragraph above stays as written, because
> "the surface is stable" was the claim and it still holds; what changed is that a second front-end
> arrived and needed a keystroke, not that the shape of the layer moved. The scope below grows with
> it, and the two pages are still owed.

The suggested scope:

- **`module-task.md`** — `TaskHost`'s D10 verb shape and the run lifecycle; the capability surface's
  two seams (`installer()` for the data tables, `privateCapabilities()` for the primitive table) and
  why the second is private; the observation-cycle protocol and `CycleLedger`, including `spend`
  beside `consume` — a separate method rather than a flag, so each call site states whether its input
  has a coordinate to authorize; the carrier shapes of Tier A/B/C; which policy the trusted Luau
  framework (`runtime/ctx.luau` plus `runtime/task.luau`) owns, and exactly where the "C++ owns
  guarantees, Luau owns policy" line falls. **Since 2026-07-30 it must also cover the second
  front-end**: `task::OperatorSession` as a sibling consumer of the same private primitives rather
  than a route into Luau, the per-generation front-end claim that makes the two mutually exclusive,
  and the `key` primitive — `ctx:key(ticket, name)` and `view:key(name)` on the Luau side, requiring
  an open cycle and spending it, with no hit ordinal and no page requirement. It must **not** restate
  the sandbox, budgets, and dual environments that `module-script.md` already covers, nor the
  operator wire protocol that `entry-cli.md` covers.
- **`module-trace.md`** — schema ownership of `umbraflow-trace/v1`; the event families (`run.*`,
  `engine.*` including `engine.key_delivered`, `task.native_call`, and the eight `framework.*`
  events from 3d on); field ordering and the rules for golden comparison; the documented non-golden
  field set; `ITraceSink`'s synchronous fallible contract and the failure-precedence rules; the
  `frontEnd` stamp (`"task"` or `"operator"`), which is part of the stamp rather than of the event
  and is also a protocol rule — the validator refuses `framework.*` on an operator stream; and the
  "audit log, not a replay log" positioning. **Since 3d it must also cover the validation state machine**: why
  `TraceStreamValidator` is owned by `TraceRecorder` (the recorder is the only path to a sink in the
  codebase, so it cannot be gone around), where the two failure kinds divide (Tier B
  `InvalidResource` for a request a project caused, `InternalInvariant` for a protocol breach, which
  spends the generation), and that the step scope is **stamped rather than checked** — from which it
  follows that a step path is **not** a unique address within a run, and `retry_attempt` is what
  distinguishes a repeat.
