# 2026-08-29 — Project handlers compose recorded Tool calls

## Decision and reason

The owner overturns Z6 of
[`a Tool is a flat call over an explicit reference`](2026-08-26-a-tool-is-a-flat-call-over-an-explicit-reference.md)
where it makes a Project Tool handler a leaf and assigns all composition to
the caller. A registered handler may call both Framework and Project Tools.

The caller is ultimately an LLM agent, not necessarily a Project-authored Luau
driver. An agent asking to identify its hand should be able to call the
Project's recognition Tool. Requiring that agent to reconstruct capture,
observe, per-rectangle line reads and the join against the card catalog defeats
the purpose of publishing the Project's knowledge as a Tool.

Interactive chunks already have Tool invocation and a catalog-derived face
that includes the Project's namespace. The missing permission is to name and
publish that composition. It does not justify restoring callbacks, implicit
observation frames or a second declaration language.

The external shape remains a name, description and flat argument schema. The
handler is implementation, not caller-supplied code; each screenshot dependency
still travels as an explicit value. Every child crosses the same admission and
durable-record boundary as an independently issued Tool.

## 1. Calls are bounded per outer Project invocation

An outer Project Tool invocation may issue at most **1024 descendant Tool-call
attempts**, shared by the whole nested handler tree. A nested Project handler
does not receive another 1024-call allowance. Refused attempts count, and
replayed calls count in the current execution attempt, so neither catching a
refusal nor replaying a prefix creates an unbounded loop of invocations.
Admission also counts the durable descendant tree against the same ceiling:
replaying a terminal Project child without entering its VM does not refund the
descendants already recorded beneath it. An over-limit proposed position may
be recorded to explain its refusal, but cannot dispatch. Restarts therefore
cannot admit work past the ceiling one cached subtree at a time.

This is a separate Project invocation ceiling, not a reinterpretation of the
interactive session's existing ceiling. Each interactive top-level call remains
an independent root. The constants live with scoped execution; no Project
descriptor member is added for them.

1024 admits the capture/recognize/join use case and matches the existing scoped
interactive bound without making nested fan-out multiply it. This ruling
explicitly gives the framework these structural limits; it is not authority to
invent additional per-Project semantic quotas.

## 2. Depth and active cycles are refused before another handler starts

At most **16 Project handler frames**, including the outer handler, may be
active. Framework leaf calls do not consume another Project frame. Attempting
to call a Project Tool whose name is already active is refused, covering direct
self-recursion and longer cycles. Repeated sequential calls after a frame
returns remain legal.

The limits solve different problems: the shared count bounds total work, the
depth bound protects synchronous dispatch, and the active-name check prevents
recursive composition from spending that stack allowance. A caller cannot
evade them by alternating Tool names or catching an invocation refusal.

## 3. Budgets belong to the outer invocation

Every handler invocation uses a fresh VM. Fresh state does not mean a fresh
outer budget: the outer Project Tool owns the elapsed window for its entire
call tree, including time spent synchronously inside children. A child's own
deadline may narrow that window, never extend its ancestor's deadline.

Nested VMs share an allocation account. Live VM allocations are charged against
the outer ceiling while each VM also obeys its own limit. Destroying a child VM
releases its live charge; starting another child does not erase allocations in
its still-active ancestors. This is a live-allocation ceiling, not a cumulative
allocation-throughput quota or a new claim to account all native provider
memory through Luau's allocator.

The call-count, depth, elapsed and allocation checks are independent. None can
be reset by entering another handler, and none replaces the ordinary admission
budgets for an actual child Tool.

## 4. Re-entry is justified by recorded children again

The invariant of
[`re-entry is keyed on recorded children`](2026-08-22-re-entry-is-keyed-on-recorded-children.md)
applies again: a handler has no filesystem, network, process, clock, random or
Host FFI escape; its effects can only be admitted, durably recorded child Tool
calls. A fresh issuing context numbers children from one for each handler.
The parent identity and child position identify a replay coordinate.

Re-entering a handler meets its recorded children at those coordinates. A
matching terminal child returns its recorded result without invoking the
provider. A different Tool or canonical argument at a recorded coordinate is a
replay refusal, and a handler that returns before consuming its recorded child
prefix is also refused. New dispatch begins only beyond the recorded frontier.
This exact-prefix rule applies at every nested handler frame.

The safety distinction is **recorded-child composition versus direct-effect
leaf**, not read-only versus mutating. A crashed composed Project call may
re-enter even when its descriptor is mutating. An in-flight mutating Framework
leaf without a durable terminal answer remains uncertain; replay does not
re-deliver it. Its enclosing handlers remain `dispatching`, without a terminal
payload, and the caller receives `ActionRejected`. The child keeps ownership
of its uncertainty; neither a success nor a caught refusal may hide it.

At a recorded coordinate, the observation-reference hash is read from the
ledger before reconstructing the child identity. This is replay metadata, not
a freshly minted observation authority. The Tool and canonical arguments must
still match. Beyond the recorded frontier, observation-consuming calls must
present a reference recognized by the current authority; a process restart does
not make an old observation live again.

An author need not promise that a handler is a pure leaf or independently
idempotent. Safety comes from the sandbox, admission boundary, recorded prefix
and the terminal classification of the leaves. Replay divergence is a hard
refusal, not a request to try the same effect with another spelling.

## 5. No new child declaration

The Project author writes **no `child_effects`, no child-name array, no separate
child profile, no observation budget and no fourth mandatory bounds array**.
The existing `required_capabilities`, `ui_action_bounds` and `effect_bounds`
cover the handler's possible subtree. Each child's descriptor must fit every
active ancestor's bounds and must independently pass ordinary policy admission.
An ancestor's admission is neither a grant to skip policy nor permission to
increase a child's own authority.

The controller's Tool-surface profile restricts its direct calls. A semantic
Project Tool can use privileged machine vocabulary internally, including the
single-line OCR primitive needed by the motivating example. Every privileged
child still requires the Operator's explicit Tool-name policy grant, as well
as all ancestor bounds. An Agent cannot call that primitive directly merely
because a handler is permitted to use it. Applying the Agent's public-vocabulary
restriction again inside the handler was rejected: it would leave semantic
recognition wrappers unable to implement their advertised operation.

The durable ledger records the Tool names and arguments that actually ran.
Static bounds describe authority; the call tree describes execution. Those are
different questions, and duplicating the tree as a hand-written list of child
names answers neither one better. Existing mutability and risk declarations
must likewise truthfully describe the composed Tool; composition is not an
escalation mechanism.

The alternatives rejected are:

- Restoring `child_effects`: it repeats Tool names and ceiling declarations
  beside existing descriptor bounds and recreates the authoring burden that
  Z6 correctly objected to.
- Adding a fourth required array under a new name: it has the same cost without
  a safety property absent from ancestor ceilings plus ordinary admission.
- Recording children without checking ancestor authority: recording explains
  what happened but cannot authorize it.
- Forcing composition into an agent's interactive chunk: it makes the caller
  rediscover Project knowledge and prevents publishing that knowledge as a Tool.
- A per-handler fresh descendant allowance or unlimited nesting: either lets
  composition multiply the caller's work or consume an unbounded native stack.

## What this supersedes

The 2026-08-26 decision keeps its bytes. Only these clauses move:

1. **Z6:** the leaf-only handler, the prohibition on handler Tool calls and
   "composition is the caller's" are overturned. The name, description, flat
   input schema, no body and no child-effect declaration remain.
2. **The opening shape:** "no nesting" no longer excludes implementation-side
   child Tool calls. No caller callback, tagged arm or implicit scope returns.
3. **What is deleted:** nested Project-handler Tool issuance is restored under
   this decision. `ChildEffectDeclaration`, `child_effects`, Tool bodies,
   `workflow.child_flow` and the accessor answer protocol remain deleted.
4. **What this supersedes, item 4:** handler-as-caller is admitted again through
   the shared runtime. This does not restore the old declaration or delegation
   model wholesale.
5. **Item 5:** handler-as-caller and the shared scoped invocation mechanism
   return. `ChildEffectDeclaration` and body-shaped dispatch do not.
6. **Item 6:** the replacement of recorded-child replay safety with handler
   purity/idempotency is overturned. The 2026-08-22 recorded-children argument
   applies, with exact-prefix completion and the bounds above.

All other clauses of the flat-call decision stand. In particular, explicit
screenshot artifacts, atomic held-input release, flat schemas, generated Tool
faces, result envelopes and independent interactive root calls are unchanged.

## Break and migration

There is no compatibility switch and no old/new child-declaration spelling.
Code that relied on every handler invocation being a terminal named refusal
must change. A Project that chooses to compose Tools uses the catalog-derived
calling surface and makes its existing descriptor bounds cover the composition;
a Project that remains a transform need not add declarations.

The scoped environment and Tool runtime protocol identities change with the
execution and replay contract. Existing source and frozen generations must be
rebuilt with the new framework's generated identities; no loader accepts the
old leaf contract as the new one. The C++ invocation, dispatch and re-entry
APIs move together with all in-tree callers. The authoring schema shape and
durable ledger schema do not gain a child-declaration field or a second form.

This change requires a newly built and published framework install before an
external consumer can use it. Editing this source tree does not modify an
already cut immutable install. The publication mechanics remain owned by the
release scripts and the generated public contract, not by this decision.
