# A framework constant a durable row pins invalidates every root when it moves

## Symptom

A format cut lands, every gate is green, and then every Operator root that
existed before the cut refuses to open:

```text
Operator root pins generation 0 to RuntimeArtifact root sha256:4b1174c7…,
and the genesis RuntimeArtifact is sha256:bf3c0e8f…
```

Nothing is corrupt. The refusing check compares two digests, and the digest it
calls foreign is the one this same code wrote into that root a release ago.

## Root cause

`H_genesis` is not data a run produced. It is the framework's own empty
`RuntimeModel`, derived from `k_genesisRuntimeModelToml`, `k_runtimeModelFormat`
and `k_runtimeArtifactFormat`, and each root **materialises** it at generation 0
of `runtime_installations` the way it materialises the staging directory. Moving
any of those three constants moves the digest, so the stored row and the compiled
constant stop agreeing.

The check that then fires was written as `recorded == currentConstant`, which
welds a **role identity** ("generation 0 names the framework's empty model") to
one era's **content identity** ("…and its bytes hash to exactly this"). Under
that reading an honest older root is indistinguishable from a tampered one, and
the refusal brands it as tampered.

The same shape exists wherever a durable row pins a compile-time constant, which
in this ledger is every artifact-root and schema identity.

## Fix

Materialisation follows the constant. Generation 0 is layout, not history, so
the migration rewrites the root's copy rather than refusing it — exactly as
`admitTheGenesisGeneration` rebuilds stored DDL when the schema identity moves.

The change that moves the constant registers the superseded digest in
`k_formerGenesisArtifactRootHashes`
(`modules/task/source/task/runtime-model-file.hpp`) in the **same** change.
`ensureGenesisGeneration` then has four branches and no fifth: absent writes the
current digest, equal passes, a registered predecessor migrates in one
transaction, anything else is refused by name.

Two rules make this a migration rather than a compatibility path:

1. **Digests only.** The superseded document's text is not kept. The old
   spelling must not live in the source, and rewriting a digest needs no bytes.
2. **Migrate every row a future pin travels through, and nothing else.** Here
   that is all of `runtime_installations` (a rollback appends the predecessor at
   a generation above 0), the `sessions` rows whose foreign key names those
   pairs, and `runtime_state.active_runtime_artifact_root_hash` — miss the last
   and a never-upgraded root opens its first session against an artifact the
   current parser refuses by declared format. Pure event logs
   (`runtime_upgrade_failures`, `release_capability_approvals`) keep their bytes:
   they record what happened, not what will be loaded.

The superseded artifact is not hand-deleted. Once nothing references it,
`reclaimUnreferencedRuntimeArtifacts` already owns it.

## Regression check

1. A fixture that constructs a root pinning the superseded digest — including a
   genesis-pinned `sessions` row and a rollback-produced installation above
   generation 0 — opens successfully, and every pin target is rewritten while the
   event logs compare byte-identical.
2. A root whose generation 0 holds an **unregistered** digest is still refused by
   name, with the database left intact. Without this case the widened branch has
   no floor.
3. The registry is pruned, not grown forever: an entry with no reproducible
   fixture must be deleted, because a guard nothing can reach is the mirror of a
   guard production does not reach.
