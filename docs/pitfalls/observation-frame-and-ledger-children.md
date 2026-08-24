# The observation frame and the ledger's call tree

`framework.screen.observe` holds a frame for the calls its body makes, ruled in
[`docs/decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md`](../decisions/2026-08-24-an-observation-frame-is-the-scope-of-its-call.md).
Two things about that shape cost a day to re-derive each time; both are recorded
here because the next step of the work walks straight into them.

## Recording a body's calls as children of the observe node needs a descriptor

### Symptom

The frame ownership works, but the calls issued inside an observe's body are
still numbered under the enclosing handler's issuing context. Re-parenting them
onto the observe node — which is what "recorded as child calls of that observe
node" asks for — is refused at admission.

### Root cause

Two independent gates, and neither is reachable without moving
`framework.screen.observe`'s descriptor:

- `OperatorCoordinator::issueToolDelegationGrant` refuses a parent whose
  `descriptor().childEffects.childToolNames` is empty ("registered no child
  effect, so it can delegate nothing"). `framework.screen.observe` declares
  none.
- Admission then requires `delegation->parentCallIdentity() == call.parentIdentity()`,
  and refuses a non-root call carrying no grant at all. So a child of the
  observe node needs a grant minted *from the observe call*, which
  `issueToolDelegationGrant` also refuses unless that call's durable row is
  still `Dispatching` — and the observe row goes terminal when the Tool answers,
  before any body runs.

### Fix

It is not a bug to route around; it is work that belongs to the single
catalog-hash move (step 2 of the ordering in
[`2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md`](../decisions/2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md)),
where observe's descriptor is written against the mechanism rather than guessed.
Until then a body cannot be expressed at all, so every observe is a frame with
an empty body and closes inside its own call.

## A frame that spans the enclosing run breaks polling loops

### Symptom

A handler that observes, acts, and observes again — the ordinary polling shape,
and what `tests/operator/test-tool-automation-loop.cpp` exercises — earns "at
most one concurrent observation frame may be open" on its second observe.

### Root cause

Letting the frame live until the enclosing handler's run exits makes every
sequential second observe look like a second concurrent frame. W1 of the ruling
is explicit that a sequential second observe simply opens a new cycle and that
polling loops are unchanged; only *lexical nesting* is refused. The two can only
be told apart by the body, so a frame whose extent is the enclosing run cannot
distinguish them and refuses the wrong one.

### Fix

The frame's extent is the scope that opened it and nothing wider. With no body
yet expressible, that scope is the observe call itself, so the frame opens and
closes inside `ProductLifecycle::Impl::answerObserveTool`. The dispatcher's
anchor, its single-frame refusal and its close-on-any-exit are all in place for
the body to take over the scope later.
