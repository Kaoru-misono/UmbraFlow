# 2026-08-25 — A body is one shape, and `hold` takes one

The four-step migration that deleted the annotation phase is complete and its
gate passes. Finishing it left the capability that motivated it unexpressible:
**engage an input, observe frames while it is engaged, release.** This settles
how to express it, and one asymmetry the migration left behind.

## Why it was blocked, and why it is not

Admission matches **a child's** declared effects against the parent's
declaration and the admitted root effect envelope:

```cpp
for (auto const& effect : effectEnvelope->effects)
{
    childEffectWithinDeclaration(childDeclaration, parentToolName, effect);
    childEffectWithinRootEnvelope(rootEffects, effect);
}
```

It iterates the **child's** effects. An observation declares none, so no mutating
Tool can be a child of an observation — which is why the `hold` arm, whose
contract is *engage and hand the lift to the Tool call that issued it*, had no
home and was deleted.

**The blocked direction is the only one that should be blocked.** The loop's job
is to constrain *declared effects*; a child with no effects makes no claim
needing constraint, and the body not running is the faithful expression of
"nothing claimed, nothing to check" rather than a gap in the patrol. The reverse
— a **read-only child under a mutating parent** — is not checked because there is
nothing to check.

That is exactly the nesting the capability needs, and it is the inverse of what
was built.

## Z1 — `hold` takes a body

`framework.input.deliver`'s `hold` arm accepts a body. Engage the input; the
body's children are read-only observations, each `screen.observe` child pinning
its own frame under the existing frame ruling, with the measuring verbs running
as grandchildren inside it; the scope closing on any exit path releases.

No new mechanism: `attachHeldInput` already makes a Tool call own a child's
engaged input, and scope-close-on-any-exit already carries the release. This
ruling only hands the general body shape to `hold`.

- **The duration is deleted.** The body's extent *is* how long the press lasts,
  and keeping a duration beside it is two spellings of one thing. The deleted
  native's press-wait-release now has exactly one spelling: observe some frames
  inside the body, and **the frames are the clock.** This is better than the
  native wait was — the framework records what was on screen while the input was
  held, so the run is verifiable, whereas a blind 800 ms hold records nothing and
  is at odds with the traceability contract.
- **An empty body is refused, specifically.** Engage and immediately release is a
  keypress, which is the `key` arm's territory, so an empty `hold` body is
  `key`'s second spelling. The refusal names it: *"a hold body is empty; press
  and release is the `key` arm."*
- **No depth constant. The admission loop is the only law.** A one-level limit
  would kill the motivating case outright — it is already two levels deep
  (`hold → observe → measure`). Depth is not a limit: the ledger's call tree
  expresses arbitrary depth, parent and child are never on the call stack, and
  nothing is re-entrant; every level is judged by the same loop against its
  parent's declaration and the root envelope. Legislating depth would be the
  framework deciding how complex a Project's flow may be.

**The admission loop itself does not change.** Adding a branch that checks
something for read-only children would add a check where nothing is checked. Its
soundness rests on "a Tool declaring no effects really does not mutate", which is
the catalog's and the Tool boundary's job and was never this loop's; write that
dependency as a comment beside the loop.

## Z2 — A body is one shape, spelled once

The two Tools' bodies are mechanically identical: open a scope, hang children
under it in the ledger's call tree, close on any exit path, pass each child
through the same admission loop. Only the scope's **meaning** differs — observe
pins a frame, hold keeps an input engaged — and meaning belongs to the Tool's
handler while mechanism belongs to the framework. That is the capabilities-not-
decisions line exactly: the framework supplies the scope mechanism and enforces
admission; what a scope means is the Tool's business.

Two copies of scope open/close plus child admission is already the two-spellings
defect today; it does not need a third body-taking Tool to convict it. When a
third arrives, its cost should be **a declaration bit on a descriptor, not a
feature.**

Granularity lands where it is true: `input.deliver` is a six-verb closed tagged
contract and only `hold` takes a body, so **the descriptor declares per arm** (a
Tool with no arms declares per Tool).

This is a **third catalog-hash move**, and it is packaged like the first two:
the descriptor field, `screen.observe`'s declaration, the `hold` arm's body and
the duration's deletion, all in one move. Landing it in two moves the hash four
times.

## Z3 — The two seams collapse into one

The `screen.observe(body)` machinery could not be given to scoped Project code,
because that seam's contract states "no yield, no coroutine suspension and no
callback back into the VM". The implementer refused to break a stated contract
quietly and built a separate task-owned seam, so only an exploration chunk can
write a body today.

**That is the migration's own "two dispatch paths" defect recurring one level
down, and the two seams collapse into one.** The contract is **rewritten**, not
violated: its clause was protecting **unstructured** re-entry — a yield, a
suspension, a bare callback escaping the call frame — each of which leaves the VM
stopped inside a stack-locked C call with framework state half-open and
unowned. A body is none of those. It is the framework's **own structured child
dispatch**, travelling the call tree rather than the call stack, with the scope
closing on every exit path. The mechanism cost of making that re-entry safe was
already paid; the task-owned seam pays it a second time.

The rewritten clause: no yield, no suspension, no unstructured callback; **a body
re-entering through the framework's own child dispatch is approved, and is the
only approved re-entry.**

The cost, stated plainly: after the collapse, a Project handler's budget
accounting — the Luau memory ceiling, the duration — must be attributed across
nested scopes, because a child observation runs *inside* a Project call and
something must own its consumption. The answer must remain **the limits the
registration declared**, never limits the framework invented. The safety story
does not live in the seam but in admission: a deny-all policy still refuses a
mutating child inside a Project body, and a policyless session's read-only
observation is still guaranteed by the existing ruling. The seam's contract
protects VM integrity, and structured dispatch preserves it.

The task-owned seam is deleted. No switch selects a seam by caller.

## Order

1. **One packaged hash move** — Z2 as the base with Z1 riding on it: the
   descriptor's body property; `screen.observe` and `input.deliver#hold` declared
   in the same package; `hold` losing its duration and refusing an empty body by
   name; the two scope mechanisms unified. **The regression case is the
   motivating case**: `hold { observe { measure } }`, with release-on-exit reusing
   the existing tests.
2. **Downstream is unblocked the moment that lands** — an exploration chunk can
   express the capability, so the consumer's critical path does not wait on
   step 3.
3. **The seam collapse.** Third, but not indefinitely deferred: it must land
   before production-side authoring begins downstream, or the claim that
   authoring and production share one vocabulary is a lie.

## Deliberately left unresolved

- Whether two inputs may be engaged at once. Whether `attachHeldInput` supports
  it is an implementation question, to be verified when a real target needs a
  chord rather than legislated in advance.
- The exact spelling of the descriptor field and its encoding into the hash —
  the implementer's choice inside the packaged move.
- The precise budget-attribution rule across nested scopes after the collapse,
  stated by whoever collapses the seams. The only constraints: the limits remain
  the ones the registration declared, and the signal remains specific.
- Whether a wall-clock wait independent of frames is ever needed. Today the
  frames are the clock; revisit when a real target proves a blind wait necessary.

## A related fact, recorded so it is not mistaken for this

The target's `CAPS` key shows a card's details only while held, and it was the
observation that first exposed this gap. **It is not what this ruling restores.**
The `hold` arm is a **pointer** hold; the `key` arm is a tap; and **no key-hold
primitive has ever existed in this repository.** The capability ruled here is the
pointer long-press with observation while held, which is the mechanism the owner
named. A key-hold would be a new verb and a separate question.
