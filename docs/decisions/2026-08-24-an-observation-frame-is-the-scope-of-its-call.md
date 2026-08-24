# 2026-08-24 — An observation frame is the scope of the call that opened it

This settles the two collisions that blocked V2 of
[`2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md`](2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md).
That document's V2 stands; this one says how.

## The collisions were made by one expression, not by the idea

Redefining `framework.screen.observe` as a holding call ran into two things:

1. **Observe-then-observe.** The cycle ledger holds at most one cycle. If an
   observe leaves its cycle open for children to measure, a second observe in the
   same run fails against that limit.
2. **A root observe has no frame to hand its close to.** The dispatcher anchors a
   live issuing context only on a call that has a parent, which is why the
   primitive attaching a held input to its owning frame refuses at a root
   position.

Neither is a contradiction in *holding a frame*. Both are made by one way of
expressing it — a flat handle plus `engage`/`disengage` verbs the Project can
call.

## The decision

**An observation frame is the scope of the observe call itself.**

An observe with measurements is written with a **body**. The measuring verbs are
issued inside that body, bound **by call position** to the innermost open frame,
and recorded in the ledger as child calls of that observe node. The frame closes
on **any** exit path from the body.

`engage` and `disengage` survive as ledger open/close events and as private
natives, but they are **not verbs in the Project's vocabulary**. Disengage is
triggered by scope exit, not by a call that can be forgotten, written twice, or
put in the wrong place.

An observe with no children is a frame with an empty body: open, resolve, close
inside the one call — word for word today's single-shot behaviour, and the same
shape in the ledger (frame open, zero children, frame close).

The frame therefore has exactly **one** owner, the call itself; it can never
outlive the call that opened it; and root and nested are the same shape. Both
collisions disappear rather than being answered.

## W1 — A run that observes twice

**The rule is that a frame never outlives the call that opened it.** A sequential
second observe therefore always arrives after the first frame has closed, and
simply opens a new cycle — exactly as today, with polling loops unchanged.

There is no "a new observe closes the previous frame" rule, and that route is
**rejected**: it dresses the ledger's single-slot implementation fact up as a
method (supersession), and it lets a distant engage change whether a local
measurement succeeds.

The only way to write "a second observe while a frame is held" is **lexical
nesting** — an observe inside an observe's body — and that is **refused**. The
refusal names the holding call's id, the limit ("at most one concurrent
observation frame"), and what exceeded it (opening a second frame).

Why refusing is not the framework deciding for the Project: it does not choose
what the Project does next, and it does not end any frame at a moment the Project
did not name — an implicit close would be that overreach. It refuses to record a
structure its ledger cannot attribute, which is the same class as refusing an
unbound Tool or a mismatched identity.

A consequence to enforce: the cycle ledger's internal-invariant error must from
now on be reachable **only by a framework bug**, never by a sequence a Project
can write.

## W2 — The owner does not vary by call position

One shape covers root and nested. The owner is the observe call's own ledger
node.

Collision 2 came from analogising a held frame to a held input. They are
structurally different, and the difference is structural rather than
conventional:

> **A held input is a leaf whose effect spans sibling calls, so it needs an outer
> owner to guarantee release. An observation frame's dependents are its own
> children, living inside its own body, so it is already the parent and owns its
> own close.**

Once the frame no longer needs to attach to an outer position, the fact that a
root has no issuing context stops mattering — nothing needs attaching there. A
held input refusing at a root while an observe works at a root is not two
readings of one rule; it is what each of the two structures separately requires.

## W3 — Consequence check

It holds, given two implementation constraints:

1. **Frame-open and frame-close are private natives.** Only the runtime's observe
   wrapper — which closes on every exit path — may call them. There is no bare
   `engage` in the vocabulary.
2. **Measuring verbs take no storable frame handle.** They bind by call position
   to the innermost open frame. A closure may escape; a frame may not. Calling a
   measuring verb outside a body earns a specific refusal — "no open observation
   frame" — and never a silent self-capture, which is precisely the semantic
   drift this change exists to kill.

Under those, no Project-writable sequence leaves a frame outliving its call, and
every close is attributable: normal body exit, body error, or an outer unwind,
all recorded on that observe node, reusing the close-on-any-exit mechanism that
already passes its gate for held input. There is no supersession event, so there
is no unattributable close.

## What changes

- `TaskHost::observe` splits into private frame-open (open the cycle, resolve
  state) and frame-close natives. The runtime's Luau layer provides the observe
  wrapper: with no body, open and close in one breath; with a body, open, run the
  body protected, close.
- The dispatcher makes the observe call **itself** the frame: it anchors an
  issuing context at that position so calls in the body take it as parent, and
  attaches close-on-any-exit. The anchoring condition is **"this call opened a
  frame"**, not "this call has a parent" — which is what makes a root work.
- The cycle ledger is untouched. A nested observe is refused specifically, by
  name, before it can reach the invariant.
- Measuring verbs bind by position to the innermost open frame. No frame handle
  enters the vocabulary.

The engine still sees a flat sequence — open, measurements, close. Parent and
child exist only in the ledger tree, and nothing is re-entrant.

## Deliberately left unresolved

- The return surface of an observe with a body — how state enters the body and
  how the body's value leaves — and the Tool descriptors of the measuring verbs.
  Both wait for the step that makes those verbs Tools, and are settled with their
  real consumer. This decision fixes ownership and binding only.
- A coroutine yielding across an open frame. The answer is whatever the held
  input mechanism already answers, since it is the same close-on-any-exit
  machinery; if it has no answer there, that is one defect in one shared
  mechanism and is fixed in one place rather than given a second rule here.
- Concurrent frames, if a real consumer ever needs them. That is a fresh design
  question to open **with** its consumer. This decision refuses nesting on the
  single-frame structure and does not pre-spend that design.
