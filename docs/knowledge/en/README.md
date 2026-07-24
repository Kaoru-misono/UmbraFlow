# Architecture Knowledge Base

This directory is a developer's navigation guide to UmbraFlow's current executable architecture: it
takes the code as it stands as the source of truth, connecting the responsibilities, key types, data
flow, design invariants, testing strategy, and extension seams of each module and composition root,
helping readers drill down layer by layer from the overall goal and locate, before making a change,
the boundary that truly owns a given piece of semantics; the authoritative architecture and
adjudicated plans remain governed respectively by
[`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) and [`docs/plans/`](../../plans/README.md).

## Recommended reading order

1. [`00-overview.md`](00-overview.md) — First build a system-wide map and understand the complete
   chain from visual evidence to strict-background actions and the JSONL trace.
2. [`module-core.md`](module-core.md) — Start from the capability kernel that has no project
   dependencies and master error handling, numeric safety, ownership, and fundamental type
   constraints.
3. [`module-domain.md`](module-domain.md) — Continue with the shared semantics of frame identity,
   coordinate spaces, target generations, detection, and observation leases.
4. [`module-vision-image.md`](module-vision-image.md) — Examine the deterministic Gray8/SAD
   recognition core, along with the quota-bounded PNG, pixel layout, and template-asset chain.
5. [`module-annotation.md`](module-annotation.md) — Understand the authoring/runtime dual documents,
   content-addressed compilation, page resolution, recognition evidence, and action authorization.
6. [`module-engine.md`](module-engine.md) — Follow how the runtime loads publications, keeps
   same-frame decisions, orchestrates ports, executes authorized actions, and records the trace.
7. [`module-script.md`](module-script.md) — As supplementary reading, cover the standalone Luau
   embedding foundation and the not-yet-closed boundary between it and the future unattended
   runtime.
8. [`module-controller.md`](module-controller.md) — Drill down into the sole Windows reusable module
   to understand WGC, target continuity, DPI, and strict-background input delivery.
9. [`entry-workbench.md`](entry-workbench.md) — From the authoring entry point, observe how the GUI,
   capture, Preview, compilation, and atomic publication compose.
10. [`entry-cli.md`](entry-cli.md) — From the product runtime entry point, observe arguments, offline
    loading, Windows adapters, the single-action flow, and the exit-code contract.
11. [`entry-m0-demo.md`](entry-m0-demo.md) — Finally, read the frozen real-machine acceptance
    foundation, distinguishing the verified WGC/input evidence from the product implementation that
    can be extended further.
