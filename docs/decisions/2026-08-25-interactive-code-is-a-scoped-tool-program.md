# 2026-08-25 — Interactive code is a scoped Tool program, not a mode

This continues
[`2026-08-24-there-is-no-annotation-phase.md`](2026-08-24-there-is-no-annotation-phase.md)
and corrects the last execution surface that still made its transport look like
an authority boundary.

## The owner's line

> Now our modes have basically all become Tool calls, so there should no longer
> be a need to distinguish explore from runtime; call the different Tools the
> situation needs.

## The decision

An interactive code chunk is a **root scoped Tool program**. It resolves the
same release-owned scoped modules as a Project Tool handler, and those modules
reach the same single VM-facing Tool Runtime primitive. The chunk may name every
Tool in the session's pinned catalog: Framework and Project Tools are one
closure, with namespace ownership deciding which catalog validates a name.

`explore` remains only the name of the interactive CLI and its queue protocol.
It is not a Luau global, a module, a capability table, an admission class, or a
runtime environment. There is no compatibility facade: chunk source imports the
scoped modules and calls the Tool it needs.

A top-level call in a chunk is an independent root Tool call. A body-taking Tool
owns the calls made from its body as children under its durable call position,
exactly as it does in a Project handler. The transport never supplies a parent
position or an ordinal.

## Why the chunk is not itself a synthetic Tool

A Framework Tool descriptor must declare the exact child Tool names, child
effect bounds, and maximum child calls it delegates. An interactive chunk may
call Project Tools whose names are supplied by the pinned registration and
therefore cannot appear in one fixed Framework descriptor. Giving a synthetic
`framework.session.chunk` Tool a wildcard child set or an all-effects envelope
would erase the authority the descriptor and the root envelope exist to carry;
changing the Framework catalog per Project would make a release-owned catalog
depend on Project content.

The root scoped program is therefore an **issuing context**, not a disguised
Tool body. Its calls enter the ordinary root admission door one by one. This
keeps exact delegation exact and does not move the Framework Tool catalog hash.

## Budgets

The code source decides no limit. A Project Tool handler runs under the memory,
instruction and elapsed limits its registration pins. An interactive chunk runs
under the memory and elapsed limits the session opener supplied; each chunk is
one fresh scoped VM and one elapsed-time window. A limit failure names the limit
and the owning chunk. Host state, ledger state and the controlled-target binding
survive between chunks; VM globals and module state do not.

## The pure resolver stays pure

RuntimeModel parsing and resolution remain a trusted, side-effect-free program.
They do not become Tool calls merely to make two C++ types look alike. The
distinction retained is between code that can issue a Tool call and code that
cannot affect the world, not between an explore phase and a runtime phase.

## Consequences

- Delete the `explore` Framework module, its project-global projection, and its
  task-owned VM adapter.
- Feed interactive chunks the same scoped Framework module closure and pinned
  Tool catalog resource as Project handlers.
- Make the interactive root door validate both Framework and Project Tool
  names, then use the existing admission and dispatch machinery.
- Keep the interactive session type and CLI because chunk-at-a-time transport,
  target lifetime and result streaming are real shapes. They grant nothing.
- Migrate examples and tests directly to `@umbraflow/tools`,
  `@umbraflow/screen`, `@umbraflow/workflow`, and `@umbraflow/audit` with no
  compatibility spelling.
