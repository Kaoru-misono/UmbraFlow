# 2026-08-24 — There is no annotation phase and no runtime phase, only Tool calls

This **wholly supersedes**
[`2026-08-24-an-authoring-session-is-a-first-class-tool-session.md`](2026-08-24-an-authoring-session-is-a-first-class-tool-session.md),
which keeps its bytes, **with one exception**: that document's ruling on
hold-with-children — engage/disengage, parent and child in the ledger's call
tree, the holding call's `ActiveRun` frame as the issuing context, and no
re-entrancy — stands unchanged and is load-bearing here for a reason given
below.

[`2026-08-24-a-policy-artifact-is-supplied-by-the-operator.md`](2026-08-24-a-policy-artifact-is-supplied-by-the-operator.md)
is untouched. This decision depends on it and extends it.

## The owner's line

> The whole architecture should not distinguish an annotation phase from a
> runtime phase. It is all just calling Tools; some of them just use different
> Tools.

Nothing in the system may encode the phase. The difference between a session
that annotates and a session that runs the product is **which Tools are in its
closure**, and `tool_closure` is already a required member of the project
registration.

## Why the superseded ruling was wrong

It produced two manifest types and two admission authorities. That shape is what
`CLAUDE.md` forbids in as many words — two spellings of one thing, plus a second
path that reaches the same dispatcher and the same ledger.

It reached that shape because the brief it answered listed only candidates that
preserved `GenerationKind {Runtime, Annotation}` and the "a generation can never
change kind" invariant. **That invariant is itself the textbook case of a limit
the framework invented.** Four facts, absent from that brief, remove every pillar
under it:

- An empty RuntimeArtifact is unrepresentable **only** because `ui_targets` and
  `surfaces` carry `minItems: 1` in the runtime schema. That is a schema choice.
- `ControllerKind` already has `Human`.
- `AgentProfile` holds a budget and a manifest hash. It is a **budget**, not an
  identity artifact, and a budget is as real for a human-driven session as for
  an agent-driven one.
- The PolicyArtifact is **operator-supplied**, so requiring a session to pin one
  costs the Project zero files.

With the pillars gone, the split's only remaining function was to encode
who-may-do-what in the type system — which is the framework deciding on the
operator's behalf.

## The security defect in the superseded ruling

Worth stating plainly, because it inverts the safety argument. That ruling gave
an annotation session **no** policy artifact and **no** budget — and an
annotation session is the one that captures the operator's screen and drives the
operator's mouse and keyboard. It is the deepest host access the system has.
Under the standing rule that **a limit must be given by whoever it protects**,
that put the session most in need of declared limits in the position of having
the fewest.

This decision fixes it: an annotating session pins the same operator policy a
production session pins, with an operator-declared budget.

## The decision

One `SessionManifestSpec`, five fields, unchanged in name and shape, filled by
every session:

- `runtimeModelArtifactRootHash` — for a session that annotates, this is
  `H_start`: **the artifact it is editing**. A brand-new project's first session
  pins the empty model.
- `operatorProtocolSchemaHash`, `projectRegistrationHash` — as today.
- `policyArtifactHash` — the operator's, the same one production pins.
- `agentProfileHash` — controller kind `Human`, with an operator-declared budget.

`GenerationKind` is **deleted outright**, not narrowed: a residual enum would be
the same two spellings in smaller type. `OperatorPolicyAuthority` admits every
session. The capability difference — screen capture, input injection, writing
into authoring state — becomes a **policy grant**.

`ui_targets` and `surfaces` get `minItems: 0`. The `1` refuses an honest value:
the true state of every project at birth. A model with nothing in it runs and
can do nothing, which is self-consistent and useless — and **whether it is
useless is the Project's business**. That `1` is also what manufactured "a new
project's first annotating session has nothing to pin", so one invented limit
was breeding a second downstream. An empty array is explicit bytes, so this does
not create an "absent means the old behaviour" reading.

## The two invariants that are integrity, not permission

The rule proposed during review — *a session must not write into the authoring
directory of the artifact it pins as its running model* — **does not survive as
stated**. Under this decision "the model I am running" and "the base I am
editing" are the same field, so that rule would forbid annotation itself. What
it was reaching for is two rules, and these are exhaustive:

**(i) Pin-at-open immutability.** All five pinned artifacts are resolved and
byte-verified when the session opens, and those bytes govern for the session's
whole life. **An edit made during a session never takes effect within that
session.** It lands in working state, crystallises as `H_end` in the closing
record, and only a later session can pin it. This covers more than the model:
the policy artifact and the agent profile are pinned the same way, so "a session
holding authoring-write rewrites its own policy" is structurally impossible —
writing new policy bytes does not change the authorisation already pinned. This
is the framework's verifiability promise, not a permission.

**(ii) Exclusive ownership.** While a session holds working state, there is no
second writer. Otherwise the closing record "session S produced `H_end`" is a
statement the framework signed and cannot verify: the `H_start → H_end` delta
must be attributable to that session's recorded actions. **Today's kind split
does not provide this guarantee at all**, so adding it here is a net gain.

Candidates considered and rejected as integrity rules: model **provenance** —
binding a hash that never appeared as any session's `H_end` is **recorded, not
refused**, because byte verification is the verification, and refusing it would
decide how a Project's state may evolve and would forbid hand-written or
imported models; **masquerade** — under one hash chain there is nothing left to
masquerade as; **forking** the same `H_start` into two chains — permitted and
recorded faithfully, merging is the Project's business; **screen capture and
input injection** — permission, granted by the operator.

### The iterate-and-test pressure point

The strongest attack on this shape: an author wants edit → test-run → edit, and
pin-at-open forbids an edit becoming a rule in the same session.

The ruling: an annotating session's Tools may read and write the working tree
freely **as data**. What they cannot do is make those bytes the **rules** the
current session is pinned to. To test-run an edit, snapshot `H_i` and open a
child session pinned to `H_i` — and the hold-with-children mechanism kept from
the superseded document (parent and child frames in the ledger's call tree) is
exactly the machinery for that. Every test-run becomes an honest, traceable
generation. The two rulings interlock rather than merely coexist.

## What `runtimeModelBinding` becomes

The kind precondition is deleted. `TaskHost::runtimeModelBinding` resolves the
hash in the artifact store, verifies the bytes, and binds. Its refusals become
specific — "hash not in store", "bytes do not match hash" — rather than "wrong
kind". The "privately finalized" gate collapses into a constructive guarantee:
the store admits only closing-record `H_end` values and explicitly recorded
imports, so "bindable" and "closed" need no check to relate.

## What is still refused, and the one honest loss

**Still refused**, by a changed mechanism: use of a capability the pinned policy
does not grant — checked at dispatch, refused by naming the missing grant and
the pinned policy hash; an artifact whose bytes do not match its hash; an
unauthorised actor, an unbound Tool, a mismatched identity.

**Newly refused**: a second writer on working state.

**Structurally impossible**, needing no refusal: an in-session edit becoming that
session's own rules.

**Newly possible, and this is the honest loss**: an operator **can** grant screen
capture and authoring-write to a production-shaped session. That was impossible
at compile time before, and one policy file can now produce a session that both
runs a model and mutates authoring state. It is a real loss of one layer of
defence in depth. It is also precisely the loss the standing line **requires**
accepting: the protected party for host access is the operator, and a framework
that refuses a grant the machine's owner wrote by hand is inventing a limit. The
grant is not silent — it is in a hashed, pinned, ledgered artifact. The mirror
gain is larger: the session with the deepest host access goes from pinning no
policy and no budget to pinning both.

## Migration

1. `schema/umbraflow-runtime-v3.schema.json`: `ui_targets` and `surfaces`
   `minItems` 1 → 0; migrate every recorded schema hash and generated product in
   the same change.
2. Delete `GenerationKind`, the two-kind invariant, and every branch and test on
   them. Rewrite `TaskHost::runtimeModelBinding` as above with specific refusals.
3. `SessionManifestSpec` keeps its name and its five fields. A session that
   annotates fills all five: `H_start` (the empty model for a new project), the
   operator's policy artifact, and a `Human` agent profile carrying an
   operator-declared budget — **an unbounded budget is an explicitly encoded
   declared value, never an absence**.
4. Add the host-access grant vocabulary to the policy artifact — screen capture,
   input injection, authoring-write, enumerated from what the code can actually
   do. Check per grant at dispatch; name the missing grant and the policy hash on
   refusal.
5. Implement pin-at-open: the five pinned artifacts resolved and verified at
   open, immutable for the session; `H_end` only in the closing record; an
   exclusive write lease on working state for the session's life.
6. Ledger: the closing record carries `H_end`, forming the `H_start → H_end`
   chain. Binding a hash with no recorded provenance records that fact
   specifically and does not refuse.
7. One authority: `OperatorPolicyAuthority` admits every session. The
   second-authority design is void.
8. Migrate the recorded manifest bytes and schema hashes in the test fixtures in
   the same change. No fixture reads two shapes.

## Deliberately left unresolved

- Whether "privately finalized" carries a publication-visibility distinction
  orthogonal to kind. Check against the code during implementation; if it does,
  it survives as a release mechanism and is out of scope here.
- The mechanism for pin-at-open — an open-time snapshot versus a write lock on
  the source directory. The invariant is ruled; the mechanism belongs to the
  implementation.
- The grant vocabulary's granularity and naming, to be enumerated from real
  capabilities and phrased for an operator.
- How an unbounded budget is encoded. The only constraint is that it be explicit
  bytes.
- How two chains forked from one `H_start` are merged. Ruled explicitly as **not
  the framework's business**; it records both chains faithfully.
