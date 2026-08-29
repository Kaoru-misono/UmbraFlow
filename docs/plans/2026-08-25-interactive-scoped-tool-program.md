# Interactive scoped Tool program

Decisions: [interactive code is a scoped Tool program](../decisions/2026-08-25-interactive-code-is-a-scoped-tool-program.md),
as superseded for bodies by
[a Tool is a flat call over an explicit reference](../decisions/2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md),
and for handler composition by
[Project handlers compose recorded Tool calls](../decisions/2026-08-29-project-handlers-compose-recorded-tool-calls.md).

## Work

1. Factor the scoped VM Tool Runtime primitive so registered Project handlers
   and interactive chunks share the same program substrate and invoke primitive.
2. Add a chunk-at-a-time scoped program front end with caller-supplied memory and
   elapsed limits, fresh VM state per chunk, scalar result transport, and
   specific limit diagnostics.
3. Replace the exploration VM's `explore` global with the scoped module closure
   and its pinned combined Framework/Project Tool catalog.
4. Route every top-level chunk call through combined-catalog root admission;
   keep handler child dispatch distinct from the removed Tool-body mechanism.
5. Delete the obsolete module, projection and task-owned native adapter; migrate
   tests, examples, generated contract and the `uf-chaos` consumer.
6. Falsify the shared substrate, Project-Tool reachability, handler child
   admission, fresh chunk state and budget ownership checks; then run targeted
   tests and the repository validation workflow appropriate to the change.

## Closed: the scoped SDK does not grow a general verb facade

The catalog-derived calling surface is governed by the flat-call decision;
handler composition is governed by the composition decision above. Neither
requires a second hand-maintained SDK facade. Consumers take the generated
calling surface and migration instructions from
[`PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).
