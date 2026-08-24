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

**Resolved 2026-08-24 by the natives-to-Tools cut.** Both gates were opened in
the one catalog-hash move, and neither was routed around:

- `framework.screen.observe`'s descriptor now declares `child_tool_names` —
  `framework.screen.census_grid`, `framework.screen.probe` and
  `framework.screen.read_lines` — so a grant can be minted from it.
- The body runs **inside the observe provider**, between the durable dispatch
  boundary and the terminal write, so the row is still `Dispatching` when the
  grant is minted. `ProjectToolDispatcher::runToolBody` anchors an issuing
  context on the observe call's own position for the body's extent. The same
  function now owns `input.deliver#hold` bodies; only the Tool-supplied close
  differs (frame release versus input lift).

**The third gate the symptom did not name, and the one that shapes the whole
vocabulary**: admission also matches a child's proposed effect against the
ADMITTED ROOT EFFECT ENVELOPE (`childEffectWithinRootEnvelope`). An observation
declares no effect bound, so its envelope is empty, so **no mutating Tool can
ever be a child of an observation**. Giving observe an effect bound to fix that
would make it a mutating call a deny-all artifact refuses, which would take
read-only screen observation away from the session that has no policy yet. So a
body holds read-only measurements only, and an input or an authoring write is
issued at the top of the run instead — which is what `explore.luau` spells.

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

The frame's extent is the scope that opened it and nothing wider. That scope is
the observe call itself, so the frame opens and closes inside
`ProductLifecycle::Impl::answerObserveTool` — and it stays that way now that a
body exists, because the body runs inside that same call. Only what happens
between the open and the close changed; the ownership did not.

## A refused root call poisons every later call under the same root request

### Symptom

A chunk whose first Tool call was refused at admission — an input under
deny-all, say — finds every later top-level call in the session refused with
"Tool call sequence predecessor has no deterministic terminal outcome",
whatever it asks for.

### Root cause

`persistToolCallPosition` writes the position before admission runs, so a call
refused AT admission leaves a durable row in `proposed` and never reaches a
terminal state. Admission then refuses the next position in that root's sequence
because its predecessor is not terminal. It is correct for a run whose calls are
one sequence; it is wrong for an annotator, whose acts are independent and where
a refusal must cost exactly the act it refused.

### Fix

One ROOT REQUEST per top-level exploration call rather than one per session:
`ProductLifecycle::Impl::issueExplorationCall` numbers its request key from
`explorationRequests`, so every top-of-run call is its own root — exactly as
every CLI verb's single call is. Calls inside an observation's body are
unaffected; they are children of that observation and are numbered under it.
