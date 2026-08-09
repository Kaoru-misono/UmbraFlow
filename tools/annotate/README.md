# Offline annotation authoring and export

`tools.annotate` is the private authoring authority. It never opens the
production Operator database and it never installs a production generation.

The private workspace is exact and non-migrating:

```text
annotation-workspace.sqlite
blobs/<prefix>/<sha256>
objects/runtime-artifacts/<runtime-artifact-root>/
.staging/
```

The deployment handoff is a separate OS/ACL root. It contains only committed
exports:

```text
<publication-id>/
  release.manifest.json
  runtime-artifact/
    runtime-artifact.manifest.json
    page-model.toml
    assets/**
```

No database, candidate document, evidence blob, frame, replay report, or
capability file is copied into the handoff.

## Trust bootstrap

Create three random bearer files outside the workspace and protect each with
the OS identity that owns its role. Every file is exact JCS with no BOM or
newline:

```json
{"capability_version":1,"nonce":"<64 lowercase hex>","principal":"human:alice","purpose":"human-review"}
```

Use `replay-runner` and `publication` for the other two purposes. The replay
policy is another protected exact-JCS file:

```json
{"frame_corpus_hash":"<sha256>","policy_version":1,"transition_corpus_hash":"<sha256>"}
```

Bootstrap once with the trusted process:

```powershell
python -m tools.annotate.trusted init --store E:\annotation-private `
  --human-capability E:\authorities\human.jcs `
  --replay-capability E:\authorities\replay.jcs `
  --publication-capability E:\authorities\publication.jcs `
  --replay-policy E:\authorities\replay-policy.jcs
```

The capability hashes and replay-policy hash are pinned in the exact SQLite
schema. An arbitrary same-purpose file is rejected.

## Agent boundary

The loopback Agent API only mutates immutable candidate revisions and
CAS-protected Agent checkpoints. Blob bytes can only be uploaded inside one of
those requests and must already be referenced by that candidate/checkpoint:

```powershell
python -m tools.annotate.serve --store E:\annotation-private --port 8765
```

It has no human-review, replay-attestation, publication, or activation route.
Human review uses `tools.annotate.trusted review`. Publication uses
`tools.annotate.trusted publish` under both the pinned replay-runner and
publication file capabilities.

Publication compiles canonical `page-model.toml`, closes over every referenced
asset, runs the fixed frame/transition replay gate, stages a RuntimeArtifact,
then performs one short SQLite transaction. That transaction checks the
candidate head and predecessor, inserts both replay attestations and the
publication, and advances `published_head`. Only after commit is the handoff
directory made visible. Startup recovery deletes stale staging/unreferenced
objects and recreates any committed handoff interrupted after the database
commit.

The RuntimeArtifact manifest has only the v1 fields
`manifest_schema_hash`, `runtime_model_schema_hash`, `page_model`, and
`assets`, encoded as exact RFC 8785 JCS bytes.
