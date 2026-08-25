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
body holds read-only measurements only, and an input or project write is issued
as an independent root Tool call instead. Registered handlers and interactive
chunks use the same structured-body adapter; neither has a private spelling.

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

One ROOT REQUEST per top-level interactive call rather than one per session:
`ProductLifecycle::Impl::issueInteractiveCall` numbers its request key from
`interactiveRequests`, so every top-of-run call is its own root — exactly as
every CLI verb's single call is. Calls inside an observation's body are
unaffected; they are children of that observation and are numbered under it.

## A first observation against a freshly bound target is refused by the ledger

### Symptom

Every `framework.screen.observe` in an exploration session terminally fails, and
the chunk that called it still reports success, because a failed Tool answer is
a value rather than a raise. A body passed to `screen.observe` never runs, so
measurements inside it silently produce nothing:

```text
body_ran=false  observe_state=terminal_failure
```

Nothing in the trace says why — the trace carries `engine.observed` and
`engine.action_found` and no Tool outcome at all. The reason is in the Operator
database, in `tool_call_history.outcome_payload`:

```json
{"failure_response":"abort","kind":"io_failure",
 "message":"Operator database write failed: CHECK constraint failed: target_generation > 0"}
```

The capture works. Frames are taken, templates match, SAD scores are real. Only
the recording fails, and it fails after the work.

### Root cause

Two halves of the tree disagree about where a target's generation counter
starts.

`Generation::initial()` (`modules/core/source/core/types/strong-id.hpp:30`)
returns `Generation{Representation{0}}`, and `TargetGeneration` both
default-constructs and `initial()`s to that zero. A target bound for the first
time therefore has generation **0**; it only reaches 1 after a `next()`, which
is a re-acquisition.

The `snapshots` table (`modules/operator/source/operator/ledger.cpp:1743`)
declares `target_generation INTEGER NOT NULL CHECK(target_generation > 0)`,
matching the convention every other counter in that schema follows —
`snapshot_revision`, `session_epoch`, `lease_revision` and
`project_observation_revision` are all `> 0`, and only `availability_revision`
admits zero.

So the very first observation of a session is the one the ledger cannot record.
It has been latent since the CHECK landed on 2026-08-11 and was unreachable
until 2026-08-25, because the only consumer had no Operator production root to
run a session against — the install door demanded a document no shipped verb
produced. Repairing that door is what made this defect reachable.

### Fix

**The counter moved, not the constraint.** `Generation::initial()`
(`modules/core/source/core/types/strong-id.hpp`) returns 1, and the `snapshots`
DDL is untouched — so there is no schema-identity migration and no data to
migrate, because the CHECK guaranteed no row with generation zero could ever
exist.

Three reasons that side is the one that moves:

- The ledger's convention is deliberate and was reaffirmed on the same branch.
  `snapshot_revision`, `session_epoch`, `lease_revision` and
  `project_observation_revision` are all `> 0`, and generation 0 now means
  *genesis — the state materialised before anything happened*. A bound target
  producing frames is not genesis.
- Relaxing the CHECK would give 0 two readings inside one schema, which is the
  two-spellings defect `CLAUDE.md` forbids.
- **Nothing read zero as "absent".** The earlier draft of this entry warned that
  every reader treating generation 0 as absent had to be found first; there are
  none. Absence is spelled `std::optional` throughout, and every comparison is
  relative, so shifting the origin changes no behaviour but the stored value.

`Generation<>` has exactly one consumer, `TargetGeneration`, so the core change
produces exactly one behavioural change.

### Regression check

No new test, and that is the point. The conformance fixture wrote
`TargetGeneration::fromValue(3)` — a hand-picked number no producer emits at a
first binding — and **that is what kept the whole suite blind**. It carries
`TargetGeneration::initial()` now, so every existing test that lands a snapshot
exercises a real first generation.

Falsified by returning `initial()` to zero: nine tests go red across the
contract, CLI and fault-injection suites, and the red reproduces the live
machine's own sentence, because the fixture's `REQUIRE` now carries the reason
it used to swallow:

```text
createSnapshot: Operator database write failed:
CHECK constraint failed: target_generation > 0
```

The general lesson is the one in
[checks that cannot fail](checks-that-cannot-fail.md): a fixture that spells a
value the real producer would never spell is a test that cannot fail at the
place it matters. Take the value from the producer.
