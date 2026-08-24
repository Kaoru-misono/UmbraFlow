# 2026-08-24 — The annotation policy is the Operator's, and `explore` takes a runtime root

This **corrects one clause** of
[`2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md`](2026-08-24-policy-is-the-axis-and-observation-holds-a-frame.md),
which keeps its bytes. Everything else that document rules stands.

## The clause being overturned, verbatim

That document's V5 listed what an exploration session is *given* rather than
required to author, and included:

> an **annotation policy artifact**, written into the project tree by the
> scaffold — visible, editable, and owned by the project. This does not violate
> the framework's line: the limit lives in the project's own file and is handed
> to the framework to enforce, rather than chosen silently on the project's
> behalf.

**It is overturned.**

## Why — the precise mechanism, not a general conflict

The clause defends itself against the **old, weaker** test: *is the limit handed
to the framework to enforce?* Yes, it is. But
[`2026-08-24-a-policy-artifact-is-supplied-by-the-operator.md`](2026-08-24-a-policy-artifact-is-supplied-by-the-operator.md)
established a **newer, binding** test:

> **A limit must be given by whoever it protects.**

The clause never answers it. The giver there is the **requester itself** — and
ghost-written by the framework's own scaffold, which makes it two violations at
once: **the framework invented the initial value, and the project owned the
permission.** That same ruling already named a permission written by the
requester and then enforced by the framework a **rubber stamp**, and concluded
that a Project supplies no policy artifact at all.

An exploration session captures the operator's screen and drives the operator's
mouse and keyboard. It is the deepest host access in the system. **Annotation is
not a special case here; it is the strongest case for the rule.**

## The corrected shape

The annotation policy artifact is **supplied by the Operator in the operator
root**, the same `policy-artifact.json` at the same address as every other
policy. `explore` gains a `--runtime` flag exactly as the production commands
have, and it is **mandatory** — no flagless special mode, because "which runtime
governs this session" must not have two spellings.

With no policy the resolution is **deny-all**, and deny-all still admits
read-only screen observation, which carries no effect bounds. What deny-all does
not admit is **input injection** or an **authoring write**.

## The onboarding claim, restated

The overturned V5 claimed "from nothing to a first exploration session is `init`
then `explore`, with zero hand-written files." The corrected claim:

> From nothing to a first exploration session is still `init` then
> `explore --runtime <root>`, with zero project-authored files — and in fact zero
> hand-written files by anyone: the genesis runtime artifact scaffolded by `init`
> gives the session its pin, the absent policy resolves to deny-all, and deny-all
> still admits read-only screen observation. What deny-all does not admit is
> input injection or an authoring write. The first **annotation stroke**, not the
> first session, is therefore where a hand-written file appears — and it is the
> Operator's `policy-artifact.json` in the runtime root, the same file production
> already requires. The annotation path adds no new authoring burden to the
> Project (still zero) and no new *kind* of burden to the Operator (the same one
> file, one more grant in it).

**Is this weaker? Yes, and it is stated rather than glossed.** The original
implied that a first session could annotate with nobody hand-writing anything,
and that half is now false. It became false because the correction is working:
that extra strength was bought by letting the requester sign its own permission.
The half that actually binds — the Project writes zero files — is untouched.

## The consequence, unsoftened

**A developer cannot annotate a brand-new project without Operator
authorisation.** Under deny-all they can look at the screen, but before the first
annotation stroke or the first injected input there must be a
`policy-artifact.json` in the operator root, written by the Operator, granting
the corresponding Tools.

The real size of that cost: `init` already scaffolds the genesis runtime
artifact, so **no production deployment has to be stood up** — what is needed is
one policy file in a root that already exists. For a solo developer the Operator
and the developer are the same person in a different hat, and the cost is thirty
seconds writing an honest file.

**The cost is accepted, and not grudgingly.** The alternative is the deepest host
access in the system — screen capture plus input injection — running under a
permission the requester's own scaffold signed for it. Making a rubber stamp
convenient is worse than making a real signature take thirty seconds.

**There is no third shape.** Any shape that lets a session act on the host with
no Operator authorisation either has the framework inventing a limit (scaffolding
a default policy into the operator root) or laundering one (a policy in the
project tree). Prompting the Operator interactively at `explore` startup and
recording the answer into the operator root is not a third shape — it is this one
with different ergonomics, and the giver is still the Operator.

## The state of the tree

**The overturned clause was never implemented.** Nothing scaffolds a policy into
a project tree today, so **no bytes need migrating**. This correction changes the
document and the not-yet-built `explore --runtime`, not existing data.

## Deliberately left unresolved

- How the "a manifest always pins a policy artifact" invariant and deny-all are
  spelled together — a distinguished deny-all artifact, or a manifest-level
  marker. An implementation detail, to be solved inside the existing invariant.
- The ergonomics of an Operator writing and extending grants — an editor, or a
  guided prompt.
- The exact filesystem relationship between the genesis root `init` scaffolds and
  `--runtime`. An implementation change, not a new authoring burden.
- Whether an observation-only session deserves a lighter admission. Not reopened;
  it already runs under deny-all.
