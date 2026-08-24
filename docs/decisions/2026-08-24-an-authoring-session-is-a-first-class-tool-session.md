# 2026-08-24 — An authoring session is a first-class Tool session, with its own manifest and its own authority

This settles the blocker that stopped the second half of the authoring/production
merge. The merge's first half — commit `54aa063`, which gave `binding_action`
six verbs and deleted the name `long_press` — landed without it. This decision
is what the second half needs before a line of it can be written.

## What was blocked, and by what

The owner ruled that the authoring surface and the production surface are both
just Tools and must be merged, and that capability asymmetry between them is a
defect rather than a design. That ruling has no decision document of its own;
this is the first one to carry it.

It cannot be carried out while `ExplorationSession` drives its own
`EngineSession` directly, outside the Operator ledger. **Input issued during an
annotation session is therefore never recorded**, which is the one thing the
framework is supposed to guarantee.

Joining the Tool Runtime requires an `OperatorPolicyAuthority`, whose `create`
takes a `task::RuntimeModelBinding`. `TaskHost::runtimeModelBinding` refuses one
for an Annotation generation:

```
a RuntimeModel binding requires a privately finalized Runtime generation
```

and `task-host.hpp` states the reason as an invariant: a generation can never
change kind, Runtime generations carry a verified RuntimeArtifact, Annotation
generations carry only an authoring directory. **An annotation session is the
step that produces the RuntimeArtifact a production session pins.** It has
nothing to pin. Every session's `SessionManifestSpec` pins five hashes and none
of them is optional.

## The decision

An authoring session becomes a full session in its own right, with a second
manifest type and a second admission authority, both of which admit **the same
six-verb vocabulary through the same dispatcher into the same ledger**.

```cpp
struct RuntimeSessionManifestSpec final   // renamed; the five fields are unchanged
{
    ContentHash runtimeModelArtifactRootHash;
    ContentHash operatorProtocolSchemaHash;
    ContentHash projectRegistrationHash;
    ContentHash policyArtifactHash;
    ContentHash agentProfileHash;
};

struct AnnotationSessionManifestSpec final
{
    ContentHash authoringRootHash;
    ContentHash operatorProtocolSchemaHash;
    ContentHash projectRegistrationHash;
};
```

`AnnotationSessionAuthority::create` mirrors `OperatorPolicyAuthority::create`
with no policy, no agent profile and no RuntimeModel binding:

```cpp
static auto create(
    ProjectIdentity const& registration,
    AnnotationSessionManifest const& sessionManifest,
    task::AuthoringBinding const& authoring,
    std::string_view exactOperatorProtocolSchemaBytes
) -> Result<AnnotationSessionAuthority>;
```

`TaskHost` gains `authoringBinding(GenerationId)`, which refuses a Runtime
generation exactly as `runtimeModelBinding` refuses an Annotation one.

### Two types, not one type with a kind

One struct carrying a kind was rejected, and it is the reading this repository
forbids: it necessarily creates fields that are meaningless when the kind is
Annotation, which is an "absent means something else" reading of a present
field. Two types whose every field is meaningful are not two spellings of one
thing — they are the honest expression, at the session layer, of a split the
architecture already made at the generation layer and called permanent.

What the user's merge ruling merged is the **Tool vocabulary and the capability
path**. It did not merge the two generation kinds, and this decision does not.

### What `authoringRootHash` claims

It claims that this session began from exactly this authoring state. It does
**not** claim the directory holds that hash for the session's duration —
rewriting the directory is what the session is for. The session's closing ledger
record carries the authoring root hash at close, so every annotation session
appears in the ledger as `H_start → H_end`, and a finalized RuntimeArtifact
gains a traceable parentage for the first time. That is recording, squarely
inside the framework's remit.

### The alternatives, and why each fails

- **Pin a provisional skeleton RuntimeArtifact and re-generate per edit.** The
  pin would be a claim about bytes the session exists to rewrite, so the pin
  itself would be false. It also pushes annotation into Runtime-kind territory,
  eroding the masquerade invariant, and it is an onboarding regression.
- **Give annotation no authority, and a small side recorder instead.** The
  separate authority is right; a side recorder is not. An asymmetry defect means
  a difference in *path, vocabulary, or record*. Two authorities admitting one
  vocabulary into one ledger is none of those. A bolt-on recorder outside the
  runtime recreates exactly the split the user ruled against.
- **Keep the private path and record from inside it.** Then every future
  capability — hold-with-children first among them — must be built twice.

## Q3: the two fields that are gone, not optional

An authoring session pins **no** `policyArtifactHash` and **no**
`agentProfileHash`. Neither exists. Pinning a manufactured "empty policy" hash
would be the framework inventing a policy on the Project's behalf, and it would
be the onboarding regression this repository has just spent a cut removing.

`operatorProtocolSchemaHash` **is** pinned. After the merge there is one
vocabulary, that schema defines it, and an annotation trace must verify against
the identical bytes a production trace verifies against — otherwise
traceability itself is asymmetric. The framework supplies it.
`projectRegistrationHash` is pinned; a registration precedes everything.
`authoringRootHash` the framework computes.

**A Project writes zero new files to get an authoring session.**

## Q4: who gives an authoring session its limits

Three givers, none invented by the framework:

1. **The project registration**, already pinned: identity and target.
2. **The act of opening the session.** A locally present person opens a session
   on a named Annotation generation. That act gives the limits: this
   generation's authoring directory only, the target window it names only, for
   this session's lifetime only. The framework enforces each, and a refusal must
   name what was exceeded and what the limit was.
3. **The framework's own safety remit**, already ruled in scope: vocabulary
   membership, refusing an unbound Tool, refusing a call after the session ends,
   and the unconditional `releaseHeldInputs` on every exit path.

An authoring session is therefore **not unlimited**. It is merely
**unevaluated by policy**, because nobody gave it a policy, and evaluating a
policy that does not exist would be the framework deciding for the Project. This
is safe: annotation is a locally privileged, human-driven activity, and the
person could press CAPS with their own hand. The framework's value here is
recording what happened, not constraining the person present.

## Q5: re-entrancy is excluded, and forbidden

The parent/child structure lives in the **ledger's call tree, not the call
stack**. `hold` splits into engage and disengage:

- the sink engages the input and returns;
- the holding Tool call's `ActiveRun` frame stays open, and **that frame is the
  issuing context and delegation grant for the child calls** — which also closes
  the gap that a Framework-side provider lambda holds only a
  `ToolCallPositionIdentity` and can issue nothing;
- each child observation is an ordinary, un-nested engine observation, which the
  cycle ledger already permits because it closes the observation cycle before
  calling the sink;
- the frame closing, by any exit path, disengages through `releaseHeldInputs`.

The engine sees a flat sequence — engage, observe, observe, disengage.
`EngineSession::observe()` is never re-entered inside `EngineSession::hold()`,
and `targetSerialization` is held only for the span of one operation. **No
re-entrancy proof is required, and an implementation that needs one is the wrong
shape.** Three things must hold instead: the lock is taken per operation, the
held input state is frame-attached data inside the sink so release-on-exit is
total, and the ledger closes the cycle before calling the sink — the last is
already true.

## The masquerade invariant is preserved and strengthened

Kind immutability is untouched. `runtimeModelBinding` still refuses Annotation,
and now `authoringBinding` mirrors it by refusing Runtime. Because the two
manifests are disjoint types, "an annotation session posing as a deployment
session" stops being a runtime kind check and becomes a compile error. Each
ledger session is self-identifying by the manifest type it pins, so a trace
reader cannot mistake an annotation run for a production one.

Annotation gains a **path and a record**, not deployability: it still cannot
obtain a `RuntimeModelBinding`, still cannot be finalized, still is not an
artifact. The reverse also holds — production still has no route to authoring
files or screenshots, because writing an observation into the authoring
directory is admitted only by `AnnotationSessionAuthority`. That is a difference
in scope granted by generation kind, not an asymmetry of path.

## What breaks, and must migrate in the same change

1. `SessionManifestSpec` → `RuntimeSessionManifestSpec`, every caller fixed, and
   any recorded manifest bytes or baselines in the tree regenerated.
2. `ExplorationSession`'s private route to `EngineSession` is deleted. All of its
   input and observation goes through a dispatcher-established run. No fallback
   branch survives.
3. `ProjectToolDispatcher`'s admission parameter generalises to one interface both
   authorities satisfy. Child-call issuance belongs to the holding call's
   `ActiveRun` frame.
4. `TaskHost` gains `authoringBinding()`. The two comment blocks at the top of
   `task-host.hpp` are rewritten: an Annotation generation opens a **ledgered Tool
   session**, and no longer describes a private path.
5. The closing ledger record of an annotation session carries the authoring root
   hash at close.
6. If `EngineSession::hold` blocks for the hold's duration, it splits into
   engage/disengage, with the duration held by the Tool-call frame.
7. Affected trace and ledger baselines are regenerated.

## Deliberately left unresolved

- **Whether annotation edits become ledgered operations.** The debt repaid here
  is input recording; the open/close hash pair already closes the parentage
  chain. Edit-level replay is ruled on when somebody needs replay.
- **A Project voluntarily giving its own authoring session a policy.** This is a
  future "the framework exposes configurables" item. Adding the slot now is
  speculative and touches the onboarding constraint.
- **A named human identity in the annotation trace.** No identity artifact for a
  person exists today, and inventing one is onboarding burden. The ledger answers
  with session, generation and registration.
- **The `operator` spelling in `operatorProtocolSchemaHash`.** After the merge the
  name is still true — it is the schema of the one vocabulary. Renaming is
  separable mechanical work and is not made a precondition here.
