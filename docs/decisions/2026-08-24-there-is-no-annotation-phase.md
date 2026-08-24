# 2026-08-24 — There is no annotation phase and no runtime phase, only Tool calls

This **wholly supersedes**
[`2026-08-24-an-authoring-session-is-a-first-class-tool-session.md`](2026-08-24-an-authoring-session-is-a-first-class-tool-session.md),
which keeps its bytes, **with one exception**: that document's ruling on
hold-with-children — engage/disengage, parent and child in the ledger's call
tree, the holding call's `ActiveRun` frame as the issuing context, and no
re-entrancy — stands unchanged and is implemented as written.

[`2026-08-24-a-policy-artifact-is-supplied-by-the-operator.md`](2026-08-24-a-policy-artifact-is-supplied-by-the-operator.md)
is untouched. This decision depends on it and extends it.

## The owner's line

> The whole architecture should not distinguish an annotation phase from a
> runtime phase. It is all just calling Tools; some of them just use different
> Tools.

## The decision

**One kind of session. One five-field `SessionManifestSpec`, its name and its
fields unchanged. One admission authority. One dispatch path. One native
vocabulary.** The entire difference between annotating and running the product
lives in `tool_closure` — a set the registration declares, the operator supplies,
and the framework enforces.

`GenerationKind` and the "a generation can never change kind" invariant are
**deleted outright**, not narrowed: an enum whose values are the two phases is
the phase the owner forbade the system to know, and nothing real is left for it
to carry. What is real is the closure (declared), the pinned hashes (recorded),
and sealed state (in the ledger).

`ui_targets` and `surfaces` get `minItems: 0`. A session that annotates pins in
`runtimeModelArtifactRootHash` **the model it is editing**; it pins the
operator's policy artifact, and an agent profile with `ControllerKind::Human`
carrying an operator-declared budget, where **unbounded is a declared value and
never an absence**.

A compile-time authoring/production split is the textbook case of a limit the
framework **invented**. A closure and a policy grant are limits the framework was
**given**.

## H_genesis

With `minItems: 0` the empty model is representable, and its content hash is a
constant: **every project in the universe shares one H_genesis.** The parentage
chain gains a natural root, and a new project's first session binds H_genesis
successfully and gets a model that can do nothing — coherent. H_genesis is
sealed by construction at project initialisation.

## Why `minItems: 1` was not load-bearing

The only thing it protected was a tautology: that a useless model is useless. A
production session pinning a zero-target model runs and can do nothing —
coherent, merely useless, and **whether it is useless is the Project's
business**. If an operator cares, that is one line of policy, not a schema
constraint. Refusing an honest value is the framework deciding that a Project's
model must be non-trivial.

It did bear weight, in the worst way: it made the genesis state unrepresentable,
and that single fact is what manufactured the case for a second manifest type.
One invented limit was breeding another downstream.

## Integrity versus permission — the exhaustive list

The test: a rule is **integrity** if and only if breaking it would make the
ledger false, or would let a session escape its own admission terms from the
inside. Everything else is who-may-do-what, and belongs to the closure and the
policy.

Two principles generate the whole list.

> **Principle A — admission is immutable and cannot be escaped from within.**
> The terms a session was admitted under are frozen for its life, and no closure
> content can thaw them.
>
> **Principle B — the ledger never records an assertion the framework has not
> verified.**

**1. Admission freeze (A).** Everything the manifest pins — the model snapshot,
the policy artifact, the registration and its closure, the profile and its
budget, the protocol schema — is immutable *to the session itself*. Any Tool,
whether or not the closure holds it, acts on the world and never on this
session's admission terms. This cannot be expressed as a closure rule, because it
must hold **while the closure does contain the relevant Tool**: a session may
author a new policy file for future sessions, but the one it was admitted under
is frozen. This also covers the budget — a session cannot raise its own ceiling
mid-run.

**2. Snapshot execution (A + B).** The running model is materialised from the
pinned content hash and never read live from the mutable authoring directory.

**The rule proposed during review dissolves here.** "A session must not write
into the authoring directory of the model it pins" is not an invariant; it is a
patch for leaky loading. Once the model is materialised by hash, that write is
harmless: the write lands in the authoring directory, execution continues on the
immutable snapshot, and the divergence between them is exactly what
`H_start → H_end` exists to record. Forbidding the write would forbid the core
loop annotation exists for — run, observe, edit, run again.

*Verified in tree*: production already works this way.
`OperatorCoordinator::openInstalledRuntimeArtifact` requires an installed
artifact pin in the database and then opens from the content-addressed
`runtimeArtifactRoot` keyed by `artifactRootHash`. So this rule is near-free
rather than the migration's largest item. Had it not been, the correct move was
to land snapshot materialisation in the migration's first commit — never a
temporary ban on the write as a transition, which would be a bridge.

**3. Parentage authenticity (B).** The closing record asserts "this session
produced `H_end` from `H_start`", and may assert it only if the framework can
show no other writer touched that authoring root during the session; otherwise
the record must state the shared-writer fact. Mechanism: a session whose closure
holds authoring Tools takes a write lease on that authoring root at open. This
cannot be a closure rule — a closure says "may call authoring Tools" and cannot
say anything about cross-session exclusion. Note what is ruled: **do not record
a false ledger**, not "forbid concurrency". The framework records and verifies;
whether concurrency is allowed is the operator's call.

**4. Attribution (B).** Every ledger record binds to the session's admitted
identity, manifest hash and controller kind; a session cannot record under
another's name. Already true today, carried forward unchanged.

**5. Sealed binding (B).** A RuntimeModel binding accepts only a **sealed**
artifact — a hash carrying a closing record in the ledger — and never an
in-flight directory. Pinning an unsealed tree would make `H_start` an assertion
about mutable bytes, violating Principle B directly. **This is what replaces the
generation-kind check.**

**6. Delegation decay (A, conditional).** Any Tool that creates a new action
context must produce one whose limits converge inside its creator's admission,
unless independently admitted by the operator. Today's parent/child call tree
inherits the session's admission and satisfies this by construction, so the rule
is **dormant** until a Tool that can spawn a session exists.

### Completeness

There is no seventh. The candidates tried and rejected — screen capture, writing
the authoring directory, driving product Tools, "annotation artifacts leaking
into deployment" — all reduce either to permission (closure and policy) or to
rules 1–5; the leak case is covered by rule 5 plus admission. Any future
candidate gets tested against Principles A and B: if it does not reduce to one of
them, it is permission and belongs to the closure.

## What `runtimeModelBinding` becomes

It drops the half that checked kind and keeps the half that asserts sealing: it
refuses a hash with no closing record, naming the hash and the missing record.
Specific signals, as always — not "wrong kind".

## What is still refused, and the one honest loss

**Still refused**: a Tool call outside the closure (the single dispatcher checks
each call against the pinned registration's closure, naming the Tool and the
closure); an actor the policy artifact does not authorise; an unbound Tool, a
mismatched identity, an unverifiable pin; tampering with this session's own
admission terms (rule 1, under any closure); binding an unsealed artifact
(rule 5); a second writer on a leased root (rule 3 — refused, or recorded
faithfully as shared); exceeding the budget, naming what was exceeded and what
the limit was.

**Newly possible, and this is the point rather than a hole**: a session whose
closure holds **both** product Tools and authoring Tools — the edit-and-observe
loop in one session. That is what the annotation work on `uf-chaos` actually
wants, and it is this merge's largest positive. Also: an authoring-produced
artifact reaches a production binding through sealing and admission, with no kind
laundering step.

**The honest loss**: the old guarantee "production has no route to screen
capture" becomes "no route unless the operator grants it, and every use is
recorded". By the standing line that is not a loss — the old guarantee was the
framework deciding for the machine's owner, and the protected party for host
access *is* the operator. The framework's duty is to make that grant **loud** in
the manifest and the ledger, not to forbid it.

## Migration

1. `schema/umbraflow-runtime-v3.schema.json`: `ui_targets` and `surfaces`
   `minItems` 1 → 0; regenerate via `generate_public_contract.py`; define
   H_genesis and seal it at project initialisation in the same commit.
2. Delete `GenerationKind`, the two-kind invariant and every branch on them.
   Replace `TaskHost::runtimeModelBinding`'s kind check with the sealing
   assertion. `product-lifecycle.cpp` shrinks accordingly.
3. Session open: the closure comes from the pinned registration; the single
   dispatcher checks the closure on every call; the screenshot and authoring
   natives become ordinary Tools, in a closure or not, and that is the only axis.
4. `AgentProfile`: a `ControllerKind::Human` session carries an explicitly
   declared operator budget — unbounded is a declared value, never a default.
   The manifest always pins a policy artifact and a profile.
5. Snapshot materialisation: confirmed already in place for production; extend
   the same path to every session rather than adding a second one.
6. The authoring-root write lease and the shared-writer branch of the closing
   record (rule 3).
7. Tests: delete the kind out of the fixtures; rewrite the annotation-side tests
   as "a session with a different closure"; **delete** the tests that assert the
   type split — they assert an invented limit.
8. `correct-doc-drift` over `docs/ARCHITECTURE.md` and the annotation design
   document.

## Deliberately left unresolved

- The exact policy lines granting the screen-capture and input Tools. The
  operator owns them; shape them the next time the policy grammar is touched.
- The field shape of the shared-writer record (rule 3's faithful-recording
  branch).
- Activation of rule 6, dormant until a session-spawning Tool exists.
- Only the two named `minItems` are ruled on. Other plural constraints in the
  schema are not relaxed with them; rule on each when it is met.
