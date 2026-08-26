# Split the Tool provider file

`modules/service/source/service/product-lifecycle.cpp` is 3335 lines and holds
four unrelated kinds of work at once:

- **fourteen `answer*Tool` providers**, from `answerNowTool` (read a clock,
  three lines) to `answerRawInputTool` (the input-authority boundary that
  resolves an observation, spends it, judges the target and posts through the
  Host delivery seam),
- the **session lifecycle** — `start`, `shutdown`, `startExplorationSession`,
  `startProjectAutomation`,
- **admission wiring** — `admitRootCall`, `runAdmitted`, `invokeTool`,
  `issueInteractiveCall`,
- **capture and observation** — `captureOpenCycle`, `openScreenshot`, `observe`.

Reading a clock and delivering a keystroke into an elevated target now live in
one translation unit, and they are not the same risk.

## What is right and must not move

The module layering. `service` depends on `operator`; `operator` does not depend
on `service`, and cannot reach `engine`, `image` or `ocr`. A Tool is *declared*
in `modules/operator/source/operator/tool-invocation.cpp`, whose bytes enter
`tool_catalog_hash`, and *performed* in `service`, which is the only side that
can touch a screen, a window or a clock. That separation is what lets admission
be checked independently of execution — `replayToolCall` answers from the ledger
before any provider runs, which would be meaningless if the layer that judges
were also the layer that produces.

Splitting must keep every provider in `service`. Nothing here argues for moving
one back.

## The defect

Layering requires the providers to be in `service`. It does not require them to
be in one file, and one file is what they are in. The dispatch is a
`if (toolName == ...)` chain whose fallthrough is `UF_UNREACHABLE_MSG`; every
Tool added since has appended to it. `tool-invocation.cpp` is large too, but it
is uniformly one kind of thing — declarations, no side effects. This file is
mixed.

## Shape to aim for

Group by what a provider is allowed to touch, which is also its risk class:

| Group | Tools |
| --- | --- |
| pure | `workflow.now`, `workflow.status` |
| screen | `screen.capture`, `observe`, `read_lines`, `census_grid`, `probe`, `crop` |
| input | the six `input.*` and the six `ui.*` |
| project store | `project.read_text`, `write_text`, `write_file` |

Four files, and a dispatch **table** keyed by Tool name rather than an `if`
chain, so adding a Tool is a row rather than a branch. The session lifecycle and
the admission wiring stay where they are; only the providers move.

Keep the pairing discipline that exists today: a catalog entry with no provider
must still abort rather than refuse silently, because a Tool the catalog
declares and nobody implements is the catalog lying, and the catalog's bytes are
signed.

## Why this is not being done now

It touches all twenty-five providers and the whole dispatch, and it has no
behavioural defect to fix — the shape is the problem. The branch already carries
several large unreviewed changes; adding a refactor of this size on top would
make the whole set unreadable. It wants its own change, on a clean tree, with
the gate run against it alone.

## Not settled here

Whether `answerUiInputTool` and `answerRawInputTool` belong in the same file as
each other. They share the delivery seam but differ in whether a model
resolves the target, which is the distinction the flat-call cut made load-bearing.
Decide when splitting, with the code in front of you.
