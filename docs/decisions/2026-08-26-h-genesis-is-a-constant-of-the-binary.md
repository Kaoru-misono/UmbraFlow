# 2026-08-26 — H_genesis is a constant of the binary, and generation 0 is layout

Supersedes the H_genesis section of
[`2026-08-24-there-is-no-annotation-phase.md`](2026-08-24-there-is-no-annotation-phase.md)
in one clause only. That file keeps its bytes; everything else it rules stands.

## The sentence that is no longer true

> With `minItems: 0` the empty model is representable, and its content hash is a
> constant: **every project in the universe shares one H_genesis.**

It was true when written and is true within one framework era. It is not true
across eras, and the reason is visible in the derivation: `k_genesisRuntimeModelToml`
opens with `schema_version`, and `genesisRuntimeArtifactManifestJcs` folds both
`k_runtimeArtifactFormat` and `k_runtimeModelFormat` into the manifest it
digests. **A format cut therefore moves H_genesis**, deliberately — the comment
at that derivation says so, and the alternative would be a stale constant
claiming to address bytes that changed underneath it.

The corrected statement: **every project sharing one framework binary shares one
H_genesis.** H_genesis is a constant of the binary, and the binary moves it.

## What that made happen

The RuntimeModel format cut from 3 to 4 moved H_genesis from
`4b1174c77b10e313df176819057d7bf0e65c4332cd57697dac0f9bad77ff22a9` to
`bf3c0e8ffbf87253b3f46ee14fb62cd0d135c4beef5857ad694d856f672e7866`. Every
Operator root created before the cut records the first of those as generation 0
of `runtime_installations`, and `ensureGenesisGeneration` refused all of them:

```text
Operator root pins generation 0 to RuntimeArtifact root sha256:4b1174c7…,
and the genesis RuntimeArtifact is sha256:bf3c0e8f…
```

Both digests reproduce exactly from the derivation, so **the refused digest was
this ledger's own handwriting from the previous era.** A root that had done
nothing wrong was refused in the vocabulary reserved for a root somebody had
tampered with.

## The ruling

**Generation 0 is layout, not history.** The ledger already said so —
"genesis is layout, not a release, and it has never been the thing a root was
upgraded to". This decision carries that sentence to its conclusion: a root
materialises the framework's constant, so when the constant moves, the root's
copy of it is migrated to follow. That is the same treatment stored DDL already
receives when the schema identity moves.

**The equality check stays and narrows.** The invariant worth guarding is
*generation 0 names the framework's empty model, and a digest the framework
never wrote is refused by name*. The invariant that was guarded — *equal to this
binary's current constant* — welds a role identity to one era's content identity,
and brands every honest older root as tampered. Under
[`2026-08-23`](2026-08-23-the-framework-enforces-limits-it-was-given.md), the
first is the ledger verifying its own handwriting and is the framework's job;
the second is a limit the framework invented.

**Standing rule.** Any change that moves H_genesis must, in the same change,
register the superseded digest in `k_formerGenesisArtifactRootHashes`, beside
the document whose edit moved it. `ensureGenesisGeneration` then has four
branches: absent → write the current digest; equal → pass; a **registered
predecessor** → migrate in one transaction; anything else → refuse by name,
which is where that refusal finally earns its wording.

This is not a compatibility path. The old spelling is never accepted for use,
only recognised so it can be migrated away — which is what "break it rather than
bridge it" requires for bytes already recorded.

## The list is pruned, not grown

A registered predecessor must have a reproducible fixture that constructs a root
pinning it and proves the migration lands on the current H_genesis. An entry
that cannot be reproduced is deleted rather than kept, because a guard nothing
can reach is the mirror of a guard production does not reach. This is the rule
the Operator ledger's own migration registry already states for a schema pair,
applied unchanged.

## What the migration rewrites, and what it must not

Rows a future pin travels through follow the constant; pure event records keep
their bytes.

Rewritten: every `runtime_installations` row holding the predecessor — not only
generation 0, because a rollback appends the predecessor at a later generation —
the matching `sessions` rows, whose foreign key is on the
`(installed_generation, runtime_artifact_root_hash)` pair, and
`runtime_state.active_runtime_artifact_root_hash`. Without that last one a
never-upgraded root opens its first session against an artifact the current
parser refuses by declared format, and "init then explore" breaks permanently.

Untouched: `runtime_upgrade_failures` and `release_capability_approvals`. No
foreign keys, pure event logs, recording what happened rather than what will be
loaded.

One `genesis_transitions` row records each migration, on the same terms as
`schema_identity_transitions`: written by production, read by nobody, and the
reason the ledger can still say it never rewrites a recorded row silently.

## Why rewriting those rows does not cost traceability

The only fact rewritten is **which era's spelling of the empty model a pin
names**. Everything about a run — its tool calls, its evidence, its epochs, its
policy hashes — is byte-identical afterwards, and the two eras' empty models are
semantically the same document: both declare nothing.

Keeping the old rows would not have preserved history either.
`reclaimUnreferencedRuntimeArtifacts` does not consult `sessions`, so the
superseded genesis directory is collected regardless, and the retained digest
becomes a dangling reference to bytes nobody holds — while every old root's
generation 0 points at an artifact the current binary can neither load nor
parse.
