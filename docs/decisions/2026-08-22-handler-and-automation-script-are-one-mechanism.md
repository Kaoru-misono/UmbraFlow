# 2026-08-22 — A Tool handler and an automation script are one mechanism

## Decision

This ruling refines, and does not reopen,
[`tools are the shared game-driving boundary`](2026-08-21-tools-are-the-shared-game-driving-boundary.md)
and
[`scoped Tool execution is a third program type`](2026-08-22-scoped-tool-execution-is-a-third-program-type.md).
Those two settled that every scoped operation crosses one Tool Runtime, and what
executes the scoped Luau. They left open what a Project *declares*, where the two
halves of that declaration live, what the loader compiles from them, and what
coordinate a run of one occupies. This decision fixes those.

### One declaration, one loader, one registrar, one invocation path

"Handler" and "automation script" are two admission producers of the same
admitted-run value, not two executables. Runtime dispatch of an admitted parent
call is one producer; Operator admission of an actor's start at the top of a run
is the other. Everything below the producer is one path.

A Project declares an entry an actor may start at the top of a run exactly as it
declares any other Tool: a Tool Catalog descriptor, plus a binding row naming the
closure entry that answers it. There is no second declaration shape and no `kind`
or `is_script` field anywhere. The test to apply to any proposal of one is: does
code branch on it? If yes it is a flag. If no it is dead data. Both are
forbidden.

What makes an entry startable at the top of a run is that the actor is admitted
to start it, which is the same authority and policy question that already exists
for an Agent-issued root call. "Being a script" is not a schema field; it is a
fact about admission.

Three things genuinely differ between the two producers, and each lives where it
cannot become a flag:

- **Who mints the run's coordinate** is resolved in the producer and arrives as a
  value. Dispatch derives the position from the admitted parent's durable row; a
  start at the top of a run mints a root coordinate. The shared invoke path never
  asks which kind it is serving.
- **What feeds the authority intersection** is the parent's
  `ChildEffectDeclaration` plus its delegation grant in one case, and actor and
  session authority plus policy in the other. Both producers emit the same
  effective-envelope value, and the intersection engine that consumes it is one.
- **Where the result row is written** does not differ at all. A root run is a
  root-positioned call, so both results are the terminal durable row of the call
  the run implements, validated against that entry's declared result schema.

An automation script is therefore a catalog descriptor like any Tool — schemas,
child-effect declaration, ceilings and budgets, all inside `tool_catalog_hash` —
with a binding row like any Tool.

### The binding lives in the registration root, outside `tool_catalog_hash`

The binding table maps a descriptor name to an entry point in the registration's
own closure. It is a member of the ProjectRegistration document, sorted and
unique by tool name, inside the registration root and outside
`tool_catalog_hash`. Adding it increments the registration format, as
[`project execution identity is closure plus environment`](2026-08-20-project-execution-identity-is-closure-plus-environment.md)
requires of any registration member-set change.

It is outside the catalog because the catalog's exact bytes are hashed and served
to callers, and the binding is neither caller-facing nor contract material. It is
inside the registration root because replay compares execution identity field by
field, and a binding outside the root digest would let a re-registration rebind a
Tool to different code mid-history with no detectable identity change — "the
exact pinned environment" would no longer pin which code answers a call. Inside
the root, registration refusal on digest inequality covers rebinding for free,
and a mid-run rebind surfaces as a named-field mismatch that stops the run. The
`Context` section below gives the full argument on both sides.

The loader gains three bind-time refusals, each of which must be exercised by a
test so that none is a dead branch:

- a binding naming an entry the closure does not export;
- a Project-provided descriptor with no binding; and
- a binding no descriptor covers.

The execution-identity attribute stored on durable rows is, or includes, the
registration root.

### One `ScopedToolProgram` per Project registration generation

The registrar compiles exactly one `script::ScopedToolProgram` per Project
registration generation. Its entry points are the union of bound entries from the
binding table, not one program per entry.

The program is authority-free by construction. Its one seam is a conduit into the
run's issuing context, which the host establishes per invoke. Per-entry variance
— the child Tool set, the ceilings, the budgets — lives in the entry's
*descriptor*, hashed in the catalog and consumed by *admission* per run, never by
the compiler. Budgets as compile inputs would be categorically the wrong layer:
policy motion would then change code identity.

Single compilation is sound only if the Tool Runtime callable bound at compile
time carries **zero** run state and routes to the issuing context of whichever
run is executing. A call's coordinate and its cancellation are per-call and
per-run inputs, not upvalues of the seam. Any run-scoped state closed into that
callable belongs in the per-invoke run request instead.

### The issuing context is keyed on the parent position and is always fresh

A run's issuing context is keyed on the position identity of the parent call it
implements. It is constructed by the dispatcher from the durable parent row and
delivered to the program; it is never influenced by the code running inside,
which the private `ToolCallPositionIdentity::create` already guarantees. On every
entry — first dispatch and re-entry alike — the context is fresh and numbers from
1. There is no persisted "next ordinal" anywhere.

Resuming a counter to skip past recorded history is not merely un-forced, it is
wrong; the `Context` section gives that argument. The four cases the fresh
constructor must cover:

- **Parent terminal.** The recorded result is returned, the run never starts, and
  its sub-tree is dead coordinate space. Per-parent numbering makes this free: no
  global counter ever observed those children, so nothing needs skipping.
- **Parent `admitted`, dispatch never began.** Dispatch runs for the first time,
  the grant is minted on the admitted-to-dispatching transition per the existing
  rule, the context is fresh, history under that parent is empty, and everything
  executes.
- **Parent `dispatching`, crashed mid-run.** Dispatch restarts and the grant is
  re-derived from the same durable parent state and descriptor — grants are
  deterministic derivations of admission inputs and never accumulate state.
  Children 1..j hit terminal rows, and child j+1 is the first position beyond
  history.
- **Zombie predecessor.** The lease and fence already in the admission set are
  what make fresh-per-entry safe against a crashed-but-still-limping prior
  dispatch minting under the same parent: the fenced-out incarnation's writes are
  refused. The fence is part of this invariant, not an adjacent feature.

The invariant that makes fresh-from-1 safe is that coordinates are *derived*,
never stored and resumed. Given the same entry, the same canonical arguments from
the parent's durable row, the same pinned environment, and the same recorded
child results, a run issues the same child sequence. That is the identical
determinism the top-level replay model already stands on, which is what makes "a
restart replays the handler on the same terms as a top-level automation script"
load-bearing rather than aspirational. Enforcement is the existing field-by-field
attribute comparison: a nondeterministic run's first divergent call names the
mismatched field and stops the run, so unsafe re-entry becomes a detected halt
and never silent divergence.

### A root-positioned call is an ordinary position row

Keying root-level calls on an absent or null parent is an "absent means
something" reading, which the house rules forbid. Under the first ruling above a
root run *is* a root-positioned call, so the coordinate exists anyway and the
optional is redundant as well as ambiguous.

A root-positioned call carries `parent_call_identity = root_identity`, naming the
`tool_root_requests` row it hangs from. A handler's child names the handler's own
position row. Those are the only two cases, and there is no third.

The root request itself gets no position row. Inventing a shadow position row per
root request would be a second spelling of the root, and the root request table
is already the root's one spelling. Because the parent coordinate therefore names
a row in one of two tables, and SQLite cannot state an alternation foreign key
across two tables, the position write path proves the named coordinate exists —
the root request row, or the parent position row — with a named refusal on either
miss.

This is a correction to what was implemented before this ruling.
`ToolCallPositionIdentity`'s optional parent identity, the scoped run request's
optional parent position, and the ledger's `parent_call_identity IS NULL` test
for a root-level call are the same absent-means-something reading, and they go
away together. Rows an earlier generation left null are migrated to their own
`root_identity` in the same change, so no reader is ever taught two shapes.

## Context

**The tuple decides the unification.** Enumerate what each side needs pinned. A
Tool needs a binding into the closure, an argument schema, a result schema, and a
`ChildEffectDeclaration` — `childToolNames`, `maximumChildSurface`,
`maximumChildMutability`, `maximumChildRisk`, `maximumChildCalls`. An automation
script, in the plan's own earlier words, needs an entry, a closed module closure,
argument and result schemas, a requested Tool set and profile, and requested
budget ceilings. That is field for field the same tuple under two sets of names:
the requested Tool set and profile are the child Tool names together with the
child surface, mutability and risk ceilings, and the requested budget ceilings
are the child-call ceiling together with the descriptor's workflow limits and
timeout policy. Two declaration shapes for one tuple is the "two spellings of one
thing" the house rules ban, and it decays the way every shim decays: each future
field must be added twice, and the next reader cannot tell whether an asymmetry
between the two shapes was intentional. The plan had already committed to
run-semantics identity — "a restart replays the handler on the same terms as a
top-level automation script" — and that demand is unmeetable if the automation
path is a second machine.

**Why the binding is not catalog material.** The registration already splits
identity into contract (`tool_catalog_hash`) and code (`plugin_environment_hash`).
The binding is the *join*: it maps a contract name to a code entry and references
both sides, so it belongs beside both rather than inside either. Three things
break if it goes into the catalog.

1. *Exact-bytes verifiability dies.* The catalog's exact bytes are hashed, and a
   caller may legitimately need existence-and-schema without being entitled to
   know what implements a Tool. With the binding inside, tool description must
   either leak it or serve a redacted projection whose bytes no longer hash to
   `tool_catalog_hash` — so the caller can no longer verify the served catalog
   against the pinned digest, which is that digest's entire job. That is two
   readings of one document.
2. *Contract churn on refactor.* Renaming a handler function or module is a pure
   implementation change. With the binding in the catalog it moves
   `tool_catalog_hash`, invalidating every pin that cares only about the contract
   — child admission intersects descriptors, and approvals and policy reference
   catalog authority. Implementation motion would force contract re-approval.
3. *Audience mismatch.* Every other descriptor field is consumed by callers and
   by admission. The binding is consumed by exactly one party at one moment: the
   dispatcher, at dispatch. Dispatcher-only data inside the caller-facing
   authority makes the document mean two things.

**Why one program per generation rather than one per entry.** Four reasons, any
one of which is sufficient. The registration carries one
`plugin_environment_hash`, singular; per-entry programs mean per-entry
environment identities with no home in the existing identity model, and would
force a redesign of the registration into a hash set and a re-answer of what
replay's "exact pinned environment" means. The closure is closed and shared, so N
per-entry programs are N copies of one closure under N identities — N spellings
of one environment. A fresh VM per invoke already delivers every isolation
property per-entry compilation could buy, so sharing the program is
compile-artifact sharing only and there is no runtime state to protect. And
`PureDataProgram` already established the shape: one program, five entry points.

**Why the issuing context must not resume a counter.** The replay contract is
that a restarted program re-executes deterministically and *re-derives* the same
ordinals: logical call *i* must mint ordinal *i* so it can match the row at
(root, parent, *i*), receive the recorded result without executing, and let the
first position beyond history execute. A resumed counter hands re-executed early
calls fresh ordinals beyond history, double-executing already-terminal effects —
the precise failure the replay model exists to prevent. Persisting the counter as
an optimisation is refused for a second reason as well: it is a second source of
truth for a value that must stay derivable, and drift between the two is
undetectable exactly when it matters.

**Why the root's parent is not an optional.** The alternative on offer was a
nullable parent meaning "this call is at the top". That is an absent value
carrying a meaning, which is the reading `CLAUDE.md` forbids, and it forces every
coordinate query to switch on presence. Once a root run is a root-positioned call
the ambiguity is not merely tolerable to remove — it is redundant, because the
run's own coordinate already names the root request. The remaining question was
only whether to manufacture a position row for the root request so that
`parent_call_identity` could carry a single foreign key. That was refused: it
would be a second spelling of the root, and the two-table proof at the write path
costs one query and names its own refusal.

## Superseded ruling parts

This decision supersedes exactly one clause of
[`scoped Tool execution is a third program type`](2026-08-22-scoped-tool-execution-is-a-third-program-type.md),
in the sentence:

> "Root call" in `@umbraflow/tools` means a parentless position under the run's
> existing `ToolRootRequestIdentity`; "child call" means a parented position
> under the same root. Scripts never mint root request identities.

The superseded word is **parentless**. No position is parentless. A root call is
a call issued by the run's own issuing context, and its position names the run's
`ToolRootRequestIdentity` as its parent coordinate; the root request keeps its
own row in its own table and gains no position row.

Everything else in that sentence, and in the ruling around it, survives
unchanged: a root call is still a call under the run's existing
`ToolRootRequestIdentity`; a child call is still a call a handler's context
issues under the same root; scripts still never mint root request identities; the
ordinal coordinate (root, parent position, child index) remains the sole replay
lookup key with every other field a compared attribute; and per-parent numbering,
the named-field divergence halt, and the seam's exclusive ownership of the child
index all stand. Nothing about the third program type, the synchronous seam,
facade authorship, or `@umbraflow/audit` is reopened.

## Consequences

- A Project has one authoring surface for executable entries. A Project Kit
  scaffold that generates "a script" and "a tool" as different artifacts
  contradicts this ruling.
- A proposed `kind`, `is_script`, or equivalent discriminator is refused at
  review under the branch-or-dead-data test, including when the change that adds
  it promises to remove it.
- `project_tool_bindings` is a required ProjectRegistration member and the reason
  the registration format moved. A registration whose catalog declares a Tool
  with no binding, or a binding with no descriptor, or a binding naming an entry
  the closure does not export, is refused at load, and each refusal carries a
  test.
- Tool description can serve the catalog's exact bytes, so a caller can verify
  what it was served against `tool_catalog_hash`. Renaming a handler entry moves
  the registration root and not the catalog hash.
- The compiler never sees a budget, a ceiling, or a child Tool set. A change that
  passes one into compilation moves code identity on policy motion and is
  refused.
- The Tool Runtime callable a program is compiled against must be run-stateless.
  A composition that closes a lease, a root identity, or a counter into it is a
  defect, not a shortcut.
- There is no persisted next-ordinal column, and a proposal to add one is
  refused.
- The lease and fence are load-bearing for replay, not merely for mutual
  exclusion. Weakening either weakens fresh-from-1 re-entry.
- `tool_call_positions.parent_call_identity` is `NOT NULL`, no query switches on
  its presence, and no code path treats an absent parent as "root". A reader
  encountering a nullable parent coordinate is looking at pre-ruling bytes or at
  a regression.
