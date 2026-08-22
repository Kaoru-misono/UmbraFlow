# 2026-08-22 — One job, one vehicle, and the pure type keeps one entry

## Decision

This ruling refines, and does not reopen,
[`scoped Tool execution is a third program type`](2026-08-22-scoped-tool-execution-is-a-third-program-type.md)
and
[`a Tool handler and an automation script are one mechanism`](2026-08-22-handler-and-automation-script-are-one-mechanism.md).
The first settled that `PureDataProgram` and `ScopedToolProgram` are two types
because they enforce two different capability contracts. The second settled that
exactly one `ScopedToolProgram` is compiled per Project registration generation.
Neither settled what is left of the pure type's contract once the Tool Runtime
has taken over project orchestration, nor how many closures one registration
carries. This decision fixes both, and states plainly the general test those two
rulings were already applying.

### The pure program type survives with exactly one entry

`script::PureDataProgram` keeps `reduce` and nothing else. `derive`, `plan`,
`next_step` and `reconcile` die with the five-function ProjectPlugin contract the
Tool Runtime replaces. The pure registrar's expected entry set becomes
`{reduce}`; the exactly-five admission check and every pin that names the other
four die with it; and the refusal that proves a reducer cannot name a scoped
module survives, retargeted at the single-entry type rather than deleted with the
four entries around it.

Deleting the pure type altogether was the option on offer that claimed to reduce
the type count. It increases it, and the `Context` section gives that argument.

### A registration carries two closures and two manifest hashes

A Project registration carries a **reducer closure compiled on the pure type**
and a **tool closure compiled on the scoped type**, each with its own module
manifest hash. Both slots are mandatory. There is no absent-means-pure reading,
no absent-means-scoped reading, and no registration that carries one closure and
infers the other.

The "ONE closure, ONE manifest hash" invariant is the thing the flip breaks,
cleanly and outright. It is not weakened, wrapped, or made conditional; it is
replaced by "two closures, two hashes", and the registration format moves once to
say so.

The bridge's exact-export admission then runs per closure rather than per
registration: `{plugin_id, reduce}` for the reducer closure, and `{plugin_id}`
plus the binding table's entry union for the tool closure. Neither set is
derived from the other, and neither closure is admitted against the other's set.

### The test: two types, or two spellings

The line between a legitimate second type and a forbidden second spelling is
this, and it is the general rule this ruling is worth citing for long after the
flip:

> **Two types are legitimate when they enforce different capability contracts
> with an executable refusal a test can make fail, and no job has two vehicles.
> Two spellings are forbidden when one job has two encodings and a reader or a
> selector must understand both.**

Both halves are required. The refusal half is what makes the difference between
the types a fact rather than a comment: reduction runs only on the pure type and
dispatch only on the scoped type because the pure resolver refuses the scoped
modules *by module name* at load, and that refusal is exercised. The
one-vehicle half is what stops the first half from licensing an ever-growing
family of types: nothing anywhere selects between the two, because reduction has
no scoped vehicle and dispatch has no pure one.

The operational form of the test, after the flip, is that **every entry name
exists in exactly one type's contract**. `reduce` is the pure type's and nothing
else's. Every bound handler entry is the scoped type's and nothing else's. A
proposal that puts one name in both contracts, or that makes a name's home depend
on a value, has failed the test regardless of how it is spelled.

## Context

Three facts closed every other door.

**The reducer's isolation is a property of the program type, not of a value.**
The resolver refuses `@umbraflow/tools` by module name, and a required experiment
pins that refusal. Any proposed "other pure vehicle" for `reduce` would have to
reproduce exactly that property, and a second type whose defining property is
identical to the pure type's *is* the pure type respelled. This is why deleting
the pure type is the two-spellings violation hiding inside the option that
advertises itself as reducing the type count: the isolation does not go away when
the type does, so it comes back somewhere, unnamed, and the reader loses the one
signature that told them what the thing in hand can reach.

**Two closures and two hashes are arithmetic, not a design cost.** The bridge
admits exactly `plugin_id` plus the compiled entry set and refuses on either
side of that equality — a declared entry the module does not export, and an
exported field the declaration does not name. The reducer's entry set and the
tool closure's entry set are disjoint, so no single closure passes both
registrars: whichever registrar it is offered to, one of those two refusals
fires. The impossibility is therefore an executable fact rather than a policy
someone chose, and the "cost" of a second manifest and a second hash was never
avoidable — the shape that avoided it was never a candidate to preserve.

**Keeping the four orchestration entries alive beside tool dispatch is the
forbidden compatibility path, verbatim.** `derive`, `plan`, `next_step` and
`reconcile` and the Tool Runtime are two generations of one job — project
orchestration — and keeping both would mean both are simultaneously reachable
with something choosing between them per project. That is the frozen definition
in
[`production reachability is the cut invariant`](2026-08-22-production-reachability-is-the-cut-invariant.md),
word for word, and no promise to remove the older one later changes what exists
in the interval.

**What the five-function contract actually was.** It was two contracts inside one
closure: one durable-data fold, and four orchestration entries. The flip
un-conflates them rather than deleting a capability. Nothing the reducer does is
lost, and nothing the four entries did survives except as Tools — which is why
"the pure type shrinks" and "the Project loses nothing" are both true at once.

**Why the neighbouring ruling's argument survives the shrink.**
[`A Tool handler and an automation script are one mechanism`](2026-08-22-handler-and-automation-script-are-one-mechanism.md)
argued for one compiled program per registration generation partly on the
precedent that "`PureDataProgram` already established the shape: one program,
five entry points". That argument turns on a program carrying an *entry set*, not
on the set having five members. With the set reduced to `{reduce}` the precedent
still reads: one program, one entry set, one environment identity. A later reader
must not treat the changed number as having removed the precedent.

## Consequences

- A proposal to keep any of `derive`, `plan`, `next_step` or `reconcile` alive
  past the flip is refused, including one that promises to delete them in a later
  commit. There is no interval in which both generations of project orchestration
  are reachable.
- A registration with one closure is not a registration. There is no reading in
  which an absent tool closure means "this Project is pure" or an absent reducer
  closure means "this Project does not fold". Both slots are stated, both are
  hashed, and a registration missing either is refused at load.
- The exactly-five entry check, its expected-set constant, and every fixture and
  pin naming the four dead entries are part of the flip's enumerated deletion
  set. The resolver-refusal test is not: it is retargeted onto the single-entry
  reducer type in the same change, so it survives the deletion of the five-entry
  pins rather than disappearing with them.
- A future proposal for a third program type is judged by the test above, not by
  a count. It is admitted if it enforces a capability contract the existing two
  do not, with a refusal a test can make fail, and if it takes over some job
  completely rather than becoming a second way to do one. It is refused if any
  entry name would then live in two contracts.
- "Fewer types" is not on its own an argument. A type whose defining property
  must be reproduced elsewhere after its deletion has not been deleted; it has
  been made anonymous.
