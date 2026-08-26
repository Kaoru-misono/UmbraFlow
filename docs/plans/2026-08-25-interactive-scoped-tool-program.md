# Interactive scoped Tool program

Decisions: [interactive code is a scoped Tool program](../decisions/2026-08-25-interactive-code-is-a-scoped-tool-program.md),
as superseded for bodies and nesting by
[a Tool is a flat call over an explicit reference](../decisions/2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md).

## Work

1. Factor the scoped VM Tool Runtime primitive so registered Project handlers
   and interactive chunks share the same program substrate; handlers are leaf
   transforms and only interactive chunks receive the invoke primitive.
2. Add a chunk-at-a-time scoped program front end with caller-supplied memory and
   elapsed limits, fresh VM state per chunk, scalar result transport, and
   specific limit diagnostics.
3. Replace the exploration VM's `explore` global with the scoped module closure
   and its pinned combined Framework/Project Tool catalog.
4. Route every top-level chunk call through combined-catalog root admission;
   delete nested dispatch and Tool-body re-entry.
5. Delete the obsolete module, projection and task-owned native adapter; migrate
   tests, examples, generated contract and the `uf-chaos` consumer.
6. Falsify the shared substrate, Project-Tool reachability, leaf-handler
   refusal, fresh chunk state and budget ownership checks; then run targeted
   tests and the repository validation workflow appropriate to the change.

## Closed: the scoped SDK does not grow a general verb facade

The flat-call ruling answers the open item. A caller sequences ordinary Tools
through `@umbraflow/tools.call`; acting and measuring verbs do not gain a second
facade spelling. `@umbraflow/screen` keeps only the ergonomics tied to screenshot
values — spelling the empty capture object, passing explicit screenshot digests,
and validating observation handles. The six raw input Tools and the three
Project-store Tools remain direct calls with their own flat schemas.

Project handlers cannot use such a facade because they are leaves and receive
no invoke primitive. The `uf-chaos` rewrite therefore migrates its interactive
caller to the flat calls and rewrites each Project handler as a transform over
explicit observation input; it is not evidence for adding another SDK layer.
