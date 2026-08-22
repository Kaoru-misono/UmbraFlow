# 2026-08-22 — Provisioning rekeys to the registration generation

## Decision

This ruling answers the question
[`the plan's contract and identity cut`](../plans/2026-08-21-project-plugin-cycle-spi.md)
left open by name — what provisions a Project whose closure is scoped — and it
depends on
[`one job, one vehicle`](2026-08-22-one-job-one-vehicle.md), which is what makes
the registration generation carry both compiled programs.

### The row survives; its provenance changes

The `project_instances` row survives **in shape**: instance identity plus the
reduced baseline. What changes is what it hangs off. Its provenance moves from
`ProjectPluginHandle` to the **registration generation**, and
`provisionProjectInstance` becomes a function of the registration rather than of
a pure plugin handle. `ProjectPluginHandle` dies with the old registrar.
`pinSession` then chains session to instance to generation, with no step in that
chain naming a superseded type.

### The baseline is refolded and proven byte-equal, or regenerated and stated

The reducer's logic survives the flip unchanged — the same authored fold,
recompiled on the shrunk pure type — so the bytes already stored in a baseline
remain meaningful rather than merely legible.

The flip commit must therefore **refold each instance's admitted-batch history
through the new-generation reducer and assert byte equality with the stored
baseline**, then re-stamp the row to the generation. That equality is the proof
that changing the vehicle changed nothing, and it is available only because the
fold is deterministic. Where it holds, "migrate" and "regenerate" are the same
act, which is why one commit can honestly claim both.

If the Journal does not retain the inputs needed to refold, the answer is not to
re-stamp unverified bytes across a trust-boundary rewrite. Regenerate from what
is retained, state the recomputed baseline as the new truth — nothing is
released, so restating is available — and **say in the commit message which of
the two happened**. A commit that re-stamps without saying which one it did has
made an unfalsifiable claim about durable state.

## Context

**Why the row cannot simply die with the registrar that writes it.** Three
things are true at once. The baseline is durable Journal state, not a cache of
something else. The reducer still needs a fold target, because
[`one job, one vehicle`](2026-08-22-one-job-one-vehicle.md) keeps `reduce` alive
as the pure type's whole contract. And sessions still need an instance: session
pinning looks up the registration-hash and instance-key pair and refuses
outright when it is absent, so a Project with no `project_instances` row can hold
no session, and a Project that can hold no session can drive nothing. Deleting
the five-function path therefore does not answer this question; it only removes
the thing that used to answer it.

**Why the registration generation is the only stable thing to rekey to.** The
handle the row hangs off today is the superseded path by definition — it is the
pure plugin admitted against all five entries. What replaces it must already
exist at provisioning time, must already be the unit the reducer is compiled
into, and must not be a second identity invented for this row alone. The
registration generation is all three: it is the unit of compilation, it now
carries the reducer as one of its two closures, and every other durable
attribute in this area is already keyed on registration identity. Any new
identity minted for provisioning would be a second spelling of the generation.

**Why byte equality is a real proof and not ceremony.** The alternative reading
is that a fold is a fold, the bytes are the bytes, and re-stamping is
bookkeeping. That reading is wrong precisely at a trust boundary rewrite: the
claim being made is that the *new* compiled reducer produces what the *old* one
produced, and nothing but recomputation can support it. A baseline is the state
every later revision is derived from, so a silent divergence there is not
detected later — it is inherited by everything.

**Why the fallback is regenerate-and-state rather than trust-and-continue.**
Where the fold inputs are not retained, the honest position is that the old
bytes cannot be verified. Carrying them forward under a new generation's stamp
would make the row assert a provenance nobody checked, which is the shape of
claim this repository refuses everywhere else. Nothing is released, so restating
the baseline costs nothing outside the tree; what it costs is a sentence in the
commit message, and that sentence is the whole audit trail for the substitution.

## Consequences

- `provisionProjectInstance` takes the registration generation. A signature that
  still takes a pure plugin handle after the flip is a surviving reference to a
  deleted path, not an equivalent spelling.
- `ProjectPluginHandle` is in the flip's enumerated deletion set, together with
  the old registrar and the old provisioning path. Nothing outside that set may
  keep a reference to it.
- `pinSession` chains session to instance to generation. A session pinned
  against anything that is not the current registration generation is refused,
  and no path infers an instance for a Project that has none.
- The flip commit contains either a refold-and-assert or a regenerate-and-state
  for every instance row, and its message says which. A flip that reports
  neither has not discharged this ruling.
- The refold-verify harness must be **built and passing on test data before the
  flip commit is written**. It reads as flip tooling, so it is easy to schedule
  into the flip itself; scheduled there, the flip is the place where the claim it
  exists to prove is first tested, and a failure at that point has no smaller
  commit to fall back to.
- A future change to the reducer's compiled environment inherits this obligation.
  Determinism is what makes the proof available; a change that makes the fold
  non-deterministic has removed the only mechanism by which a baseline migration
  can be checked, and must be refused on that ground before any other.
