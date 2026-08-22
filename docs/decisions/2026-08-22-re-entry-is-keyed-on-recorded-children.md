# 2026-08-22 — Re-entry is keyed on recorded children, not on mutability

## Decision

This ruling refines, and does not reopen,
[`a Tool handler and an automation script are one mechanism`](2026-08-22-handler-and-automation-script-are-one-mechanism.md),
whose "parent `dispatching`, crashed mid-run" case already requires a crashed
dispatch to restart, replay its recorded children, and execute only past the
recorded frontier. It fixes which calls that case covers, and answers the
question the live plan's restart section deferred by name to whoever landed
mutating handler dispatch.

### A mutating Project handler may re-enter

A mutating Project Tool handler left `dispatching` by a crash is re-entered, not
classified uncertain, under this invariant:

> A scoped handler's only channel to the world is a bound tool call; every bound
> call is an admitted, durably recorded child; on re-entry, a child call matching
> the next recorded child consumes the recorded result without live dispatch;
> live dispatch is permitted only past the recorded frontier; a divergence from
> the record refuses the frame loudly.

Under that invariant re-execution is effect-free up to the frontier, and "no
unrecorded effect" is enforced **structurally** rather than by audit: scoped Luau
has no other capability, so there is no path by which a handler could have acted
outside its own recorded children.

### The real distinction

**The distinction is not read-only versus mutating, and not Project versus
Framework. It is composed-of-recorded-children versus direct-effect leaf.**

Re-entry safety is compositional. A frame whose entire effect surface is its own
recorded children re-enters safely, whatever its declared mutability, because
every effect it could have caused already carries its own durable
classification. The composition bottoms out at leaves.

### The one non-replayable atom

The atom that cannot be replayed is a **leaf mutating provider in flight**: a
recorded child with no recorded result, where the world may or may not have
changed and no record can say which. That child stays `possible` and needs its
own protocol — an idempotency key, an effect-confirmation handshake, or operator
adjudication.

The parent handler above it re-enters, replays to that child, and **inherits its
unresolved state rather than re-issuing it**. Inheritance is the correct
behaviour, not a degraded one: the parent has learned nothing the child did not
know.

The restart gate is therefore re-keyed to refuse only that atom. A mutating
Project call left `dispatching` survives as `dispatching`. Today's
reclassification of it to `possible` is exactly what makes the gate's
mutating-admit path unreachable.

### The pinning test lands in the same change

Kill after child *N*'s effect and its record; restart; prove no duplicate effect
and prove completion; then force a divergence and prove the loud refusal. **All
of it before any production mutating provider exists.** The alternative is silent
inheritance of an untested property, and the flip must not be where this is
discovered.

## Context

**Why the current classification is a dead branch by construction.** The restart
path reclassifies an unanswered mutating dispatch to `possible`, and the
admission gate then refuses to admit a mutating call in that state. So the gate's
mutating-admit path cannot be reached: the classification guarantees it. That
survives the dead-branch rule only on the technicality that an admit is not a
check — which is precisely the shape that gets inherited silently, because
nothing goes red when a later change starts depending on it.

**Why the property was mis-attributed to read-only-ness.** The earlier cut
allowed re-entry for a read-only Project-provided call and refused it everywhere
else, and the reasoning given for it was correct as far as it went: such a call
declares no external effect of its own and is answered by a handler whose
external work is entirely its own recorded children. But the second clause is
what does the work, and it is equally true of a mutating handler — a mutating
handler's mutating work is *also* child calls, each carrying its own durable
classification. `mutating` was standing in for a property it correlates with
rather than the property itself, and the correlation breaks at exactly the case
this ruling decides.

**Why the discriminator must be decided rather than inherited.** The restriction
that keeps a mutating handler from re-entering today lives entirely in what the
recovery pass converts; the re-entry path itself refuses on provider kind alone
and would admit a mutating Project call that reached it. A change that gives a
mutating handler a dispatch path therefore widens re-entry **by default**, with
no line of code expressing the decision and no test failing to announce it. That
is why this is a ruling landed ahead of the capability rather than a note left
for the change that needs it.

**Why the leaf is where it bottoms out, and why that is enough.** A frame is
replayable when nothing outside the ledger can have happened that the ledger does
not already classify. For a composed frame, the ledger classifies every child. For
a direct-effect leaf in flight, the ledger classifies nothing — that is the
definition of the uncertain state. So the boundary is not a policy choice about
which kinds to trust; it is the point at which the ledger stops being able to
answer. Every frame above the leaf inherits the leaf's uncertainty and adds none
of its own.

**What a Framework-provided call is, under this reading.** It is a direct-effect
leaf from this side's point of view: it is answered by a provider this side
cannot re-run deterministically, so an unanswered one has no recorded child
standing between the record and the world.

Being a leaf is half of what makes a call uncertain and never the whole of it.
A Framework-provided call **whose descriptor declares it mutating** keeps the
uncertain classification, and it keeps it for that reason and not because it is
Framework's. A Framework-provided call whose descriptor declares it *read-only*
declares no external effect for a delivery to be uncertain about, so re-running
its provider delivers nothing twice; it survives as `dispatching` and re-enters
alongside every composed call. Classifying it uncertain would invent a barrier
about an effect that was never claimed, and only a mutating call can be
reconciled, so that barrier would be one nothing could lift.

## Consequences

- Restart classification is re-keyed onto a conjunction of two properties, and
  neither half is sufficient alone. A call left `dispatching` is converted to
  `possible` only when it is a direct-effect leaf **and** its descriptor
  declares it mutating. A mutating Project-provided call survives as
  `dispatching` and is re-entered, because it is composed; a read-only leaf
  survives as `dispatching` and is re-entered too, because it declares no
  effect. Reading either half as sufficient inverts the rule in one direction or
  the other.
- The rule is written once, and both places that enforce it are derived from
  that one statement. The predicate the re-entry gate calls to refuse is the
  same predicate the restart's row filter is **generated** from, evaluated
  across the closed set of (answerer, mutability) pairs so that every pair the
  predicate calls unrecordable becomes one disjunct of the filter. The
  classification a restart applies and the shape a re-entry refuses therefore
  cannot disagree — not because two statements are kept in step, but because
  there is only one statement. A hand-written SQL copy of the rule beside the
  predicate is the drift this ruling is most exposed to, since the two are read
  months apart and only a crash exercises them together; it is forbidden for
  that reason. The same closure applies to the answerer set: which answerers
  reach the world directly is a closed table joined to the durable
  `provider_kind` vocabulary in one place, so adding an answerer is a decision
  taken at that table rather than a default inherited at recovery time.
- The admission gate's mutating path becomes reachable, and must therefore be
  exercised. A gate branch that was unreachable under the old classification and
  is left untested under the new one is a check that cannot fail, now with a live
  caller.
- A re-entered mutating handler that meets an unresolved child inherits that
  child's state and does not re-issue it. A re-issue at that point is a duplicate
  effect, and the fact that the parent is replaying is not authority to produce
  one.
- Nothing about uncertain outcomes moves with this. A `possible` classification
  still cannot be converted to a rejection, moved to another parent or sequence,
  or retried, and it still freezes mutation for the root tree until reconciliation
  resolves it. What changed is which calls acquire that classification, not what
  it means.
- The crash-replay pin — no duplicate effect, completion after restart, loud
  refusal on forced divergence — lands with the gate re-key, not with the first
  production mutating provider. It reads like mutating-provider work and it is
  that work's precondition.
- A future provider that claims to be replayable must say what makes it so. The
  only two answers this ruling recognises are "its effects are recorded children"
  and "it carries its own idempotency or confirmation protocol". "It is usually
  safe to repeat" is not one of them.
