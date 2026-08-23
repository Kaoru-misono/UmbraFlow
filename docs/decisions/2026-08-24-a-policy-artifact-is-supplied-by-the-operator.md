# 2026-08-24 — A PolicyArtifact is supplied by the Operator, and a Privileged surface needs a named grant

This supersedes two earlier decisions in one respect each, and both keep their
own bytes:

- [`2026-08-09-policy-is-operator-owned.md`](2026-08-09-policy-is-operator-owned.md)
  ruled that "Projects may supply a content-addressed policy artifact as
  deployment input. Only Operator parses and evaluates it." The second sentence
  stands. **The first is overturned**: a Project supplies no policy artifact at
  all. That decision was self-consistent when written — it made the artifact
  Operator-*evaluated* — and the flaw it left was that Operator-evaluated is not
  Operator-*written*.
- [`2026-08-23-the-framework-stops-interpreting-project-state.md`](2026-08-23-the-framework-stops-interpreting-project-state.md)
  listed `policy_artifact` among the deployment document's top-level members and
  left it there. It is deleted. Everything else that decision ruled stands.

## The line this applies

[`2026-08-23-the-framework-enforces-limits-it-was-given.md`](2026-08-23-the-framework-enforces-limits-it-was-given.md)
says the framework enforces limits it was **given** and does not invent them. It
did not say who gives which limit, and that gap let one limit be given by the
wrong party. The rule that closes it:

**A limit must be given by whoever it protects.**

A Project declaring its own VM memory ceiling is correct: the limit constrains
the Project's own execution and the Project is the protected party. A
PolicyArtifact governs **who may act on the host machine**, and the protected
party there is the machine's owner — the Operator. A permission written by the
requester and then "enforced" by the framework is a rubber stamp, not
enforcement.

## What was inverted

Both the code and the schema called the artifact Operator-owned:
`effective-plan.hpp` said "the Operator-owned PolicyArtifact still decide[s]
whether those effects may be admitted", and the deleted `policy_artifact`
`$comment` said that when absent, production uses "the Operator-owned deny-all
artifact".

But the only supply path was the Project's: an optional path member inside
`umbraflow-project.json`, whose bytes the deployment loader read out of the
project directory. The word `policy` appeared in no CLI flag on any verb, so
there was no Operator-side way to hand one in at all.

**The Operator could only deny.** The deny-all default was its entire
contribution, and every permission came from the party asking for it.

## The ruling

**The artifact is read from `<runtime>/policy-artifact.json`, under the Operator
production root, and from nowhere else.** Absent still means the Operator-owned
deny-all artifact; a non-regular file, an oversized one, or a read failure is
refused by name.

A CLI flag was considered and rejected, and the reason is the rule itself: a flag
is written by whoever types the command line, and for `umbra-flow invoke` that is
the requester. A flag would have reproduced the inversion in a new spelling. The
production root is the one directory the machine's owner administers — it already
holds the Operator database and the installed runtime artifacts — and it is
already opened by every verb that needs a policy, so nothing is chosen per
invocation and no two verbs can disagree about where the policy lives.

**`policy_artifact` is deleted from the deployment document**, along with its
loader and `LoadedProject::policyArtifactBytes`. No fallback reading: the only
consumer never declared it, so there is no case for a compatibility path.

**A Privileged-surfaced Tool is refused unless the policy names it.** The policy
document gains a required `privileged_surface_tools` array — empty is a
statement, and the deny-all artifact emits `[]`. The refusal fires at admission,
in the same block as the existing surface and approval checks, worded *Operator
policy grants no Privileged surface to tool X*.

The framework has no standing to rule that a Script actor may **never** reach the
Privileged surface; that would be the framework deciding for the Operator. Its
job is that **without a grant, it refuses**. A Project keeps declaring
`surface: privileged` and the declaration is still parsed — it is an honest
classification that now grants nothing by itself.

## What the implementation found

**The brief's premise was half wrong, and the correction is worth keeping.** A
top-level call's surface was not checked against *nothing*: an Agent was already
constrained, because `ControllerProfile::semanticToolsOnly` is true for `Agent`
and the profile predicate ran at admission. The hole was Script and Human, for
whom that predicate is vacuous. The fix is unchanged; the description of what was
broken is.

**One shape change was forced.** The policy was reachable at admission only
through a mutation's authority, and a read-only Privileged Tool carries no
mutation — the example fixtures contain exactly such a Tool. So the policy
authority moved up from `ToolAdmissionRequest::Mutation` to
`ToolAdmissionRequest` itself as a required member. The alternative, a
`session_policies` column written at session pin, was rejected because any DDL
change moves the operator database schema identity and invalidates all eighteen
registered migration source identities with their reproduction fixtures.

**A latent fixture bug became load-bearing.** A test helper rebuilt an Agent's
session manifest by regenerating policy bytes rather than using the store's own,
which was invisible while one global artifact existed and silently pinned an
Agent session to a different policy once grants became per-store.

## Deliberately not done

The **offered** set is unchanged: a Privileged Tool the Operator did not grant is
still offered to a Script or Human and refused when presented. Offering and
admitting are separate predicates by design and neither substitutes for the
other, and reading the policy in the offer path would be the second scattered
check. Narrowing the offered set is a separate decision.

`umbra-flow upgrade` reads the artifact and never verifies it — its session
manifest attests to the hash but builds no policy authority, so a malformed file
is caught later, at `start`. That is pre-existing, and it predates this change:
the project-supplied bytes were unverified there too.

One consequence is worth stating because it is silent: a Framework Tool newly
declared Privileged becomes unreachable on every production root until an
operator writes a grant. That is the intended behaviour of the rule, and it will
surprise whoever adds such a Tool.
