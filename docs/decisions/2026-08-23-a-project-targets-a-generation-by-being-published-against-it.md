# 2026-08-23 — A Project targets an SDK generation by being published against it

## Decision

A Project does not declare the SDK generation it targets. It targets a
generation by being published against that release's bytes, and the derived
`plugin_environment_hash` pinned into its registration is the strongest
available spelling of that fact.

The registration carries the derived environment digest and nothing else, and
`ProjectGenerationRegistrar::registerGeneration`'s exact-inequality refusal is
the whole contract. No `sdk_generation` member exists, and none will be added.

The earlier wording in the cycle-SPI plan — "A Project declares the SDK
generation it targets" — was an abandoned direction and is struck rather than
implemented.

## Context

The plan carried this as an explicitly unresolved open question blocking WP11,
the work package that regenerates `docs/PUBLIC-CONTRACT.md` and publishes a
matching release. Two readings survived: that the phrase was stale prose, or
that it was an unimplemented requirement under which a Project names a
generation, the publisher resolves that name to release bytes, and the digest is
pinned from them. The second reading would add a registration member and
therefore increment the registration format, rewriting every authored document
and fixture — while a downstream consumer waits on this repository publishing.

## The forcing argument

The question is not which reading is more attractive. The authored member
cannot be joined to anything, and that settles it.

Naming a generation, resolving that name to release bytes, and deriving the
digest from those bytes is **one production chain**. A check that the authored
name agrees with the pinned digest therefore compares one producer's output
against itself, which is the shape
[`checks that cannot fail`](../pitfalls/checks-that-cannot-fail.md) names
directly under "expected and actual come from one producer". No test could make
such a check red, so it would not be a check.

The existing check has two genuinely independent sources, which is what
[`a join needs two independent sources`](2026-08-22-a-join-needs-two-independent-sources.md)
requires. At publication, `currentProjectPluginEnvironmentHash()` derives the
digest from **the release the publisher is itself running** and writes it into
the registration. At registration, `registerGeneration` re-derives it from **the
release the executing side is running** and refuses on inequality. The two
values are produced at two moments and can come from two different binaries, so
a publisher and an executor on different releases turn the check red. That is a
declaration joined to behaviour; a name joined to a digest derived from that
same name is not.

An authored member with no falsifiable join has only two fates, and the plan's
own criterion in section 5.1 forbids both. Either no code branches on it, and it
is dead data; or the publisher reads it and chooses between generations' release
bytes, and it is a selector —
[`production reachability is the cut invariant`](2026-08-22-production-reachability-is-the-cut-invariant.md)
and the registration schema's own `$comment` both state that a format or
generation field is an identity assertion inside one reader and never a key
anything dispatches on.

There is also nothing in the tree for such a name to resolve against.
[`project execution identity is closure plus environment`](2026-08-20-project-execution-identity-is-closure-plus-environment.md)
fixes the module VM, the registrar, the kit and the generated public contract as
**one release boundary**: the publisher's own bytes *are* the generation the
Project targets. Adding a name beside the digest is two spellings of one fact,
which `CLAUDE.md`'s "break it rather than bridge it" forbids outright.

## What this does not settle

Section 4.3 lists "SDK generation and Unicode data version" among what enters
the environment identity, but `currentProjectPluginEnvironmentMaterial()` emits
no generation literal: the identity is carried by module source hashes and
contract literals. That is a real gap in the derived material and is recorded in
the plan's findings. The remedy is to enrich what is derived, never to add an
authored field — enriching the derived material keeps both sources independent,
and an authored field destroys that independence.

Cross-generation publication — a publisher targeting a release other than its
own — is the one capability this ruling forecloses. Nothing is released, no
second generation exists, and the single release boundary above forbids it, so
adding the member now would be preserving an option at the cost of a
falsifiable check. Should the capability ever be wanted, what it needs is a
selector inside the publisher, and that is a different question to be ruled on
then.
