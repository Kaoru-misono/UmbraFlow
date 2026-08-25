# Interactive scoped Tool program

Decision: [interactive code is a scoped Tool program](../decisions/2026-08-25-interactive-code-is-a-scoped-tool-program.md).

## Work

1. Factor the scoped VM Tool Runtime primitive so registered Project handlers
   and interactive chunks execute the same adapter and the same structured-body
   re-entry contract.
2. Add a chunk-at-a-time scoped program front end with caller-supplied memory and
   elapsed limits, fresh VM state per chunk, scalar result transport, and
   specific limit diagnostics.
3. Replace the exploration VM's `explore` global with the scoped module closure
   and its pinned combined Framework/Project Tool catalog.
4. Route every top-level chunk call through combined-catalog root admission;
   retain ordinary child dispatch inside Tool bodies.
5. Delete the obsolete module, projection and task-owned native adapter; migrate
   tests, examples, generated contract and the `uf-chaos` consumer.
6. Falsify the shared adapter, Project-Tool reachability, body parentage, fresh
   chunk state and budget ownership checks; then run targeted tests and the
   repository validation workflow appropriate to the change.

## Open: whether the scoped SDK grows a verb facade

Four scoped facades exist — `@umbraflow/audit`, `@umbraflow/screen`,
`@umbraflow/tools`, `@umbraflow/workflow` — and none of them covers a verb that
acts or measures. `screen.luau` wraps only the observation handle, so
`framework.screen.read_lines`, `framework.screen.probe`,
`framework.screen.census_grid`, all six arms of `framework.input.deliver` and
`framework.project.write`'s `capture` arm are each written as a bare
`tools.call` with an inline argument table. That is one honest spelling and it is
correct as it stands; it is also three lines where the deleted `explore` module
took one.

This is deliberately not settled here. Deciding it now would design an
ergonomics layer one change after deleting the last one, on no evidence, and it
would move `scopedToolEnvironmentHash` for a guess. **Decide it when the
`uf-chaos` hand reader is rewritten against this cut** — that rewrite is the
first real body of chunk code on the new spelling and will say which verbs are
actually written often enough to earn a facade.

Two constraints on whatever is decided. Any facade must be reachable by a
registered Project handler on identical terms, or it reintroduces the capability
asymmetry this cut deleted; and it is one packaged move of
`scopedToolEnvironmentHash`, covering every verb admitted at once rather than
input first and measurement later.
