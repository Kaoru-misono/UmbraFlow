# Offline annotation authoring and export

`tools.annotate` is the private authoring authority. It never opens the
production Operator database and it never installs a production generation.

The private workspace is exact and non-migrating:

```text
annotation-workspace.sqlite
.workspace.lock
blobs/evidence/<prefix>/<sha256>
blobs/runtime-assets/<asset-type>/<prefix>/<sha256>
objects/runtime-artifacts/<runtime-artifact-root>/
replay-bundles/<prefix>/<bundle-id>
.staging/
```

Evidence and deployable assets never share a directory: the kind is part of the
path, so a blob cannot be reached through the wrong namespace. `<asset-type>` is
`template_png` or `template_webp`, and `<prefix>` is the first two hex
characters of the digest.

The schema pins four authoring capability roots: `workspace_database`,
`candidate_workspace_root`, `evidence_blob_root` and `replay_bundle_root`.
Three of them never leave the workspace. The fourth is different by design —
`candidate_workspace_root` is where publication stages a committed
RuntimeArtifact, and publication copies that artifact's files into the handoff
one by one, verifying the export against the release manifest. Its contents
travel; the root itself does not.

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

It has no human-review, replay-bundle, replay-attestation, publication, or
activation route. Human review uses `tools.annotate.trusted review`.
Publication uses `tools.annotate.trusted publish` under both the pinned
replay-runner and publication file capabilities.

## Replay Bundle

A Replay Bundle is the offline closure a project/operation replay runs from:
one baseline event, the Journal prefix after it, the structured observations,
the Operation rows, and the session manifest they happened under. Frames are
optional; when a bundle keeps them it must name the window it keeps them for,
which the workspace bounds at 30 days. A frameless bundle still supports audit
and can never stand in for a frame replay.

```powershell
python -m tools.annotate.trusted record-bundle --store E:\annotation-private `
  --replay-capability E:\authorities\replay.jcs --bundle E:\runner\bundle.jcs
```

The runner supplies the closure; the workspace mints the `bundle_id` as the
content address of that closure, validates the whole document against
`schema/umbraflow-annotation-workspace-v2.schema.json` through the official
Draft 2020-12 validator, and writes it under `replay-bundles/`. Every
observation and frame must already be an evidence blob, and those blobs are
then held against collection for as long as the bundle exists.

## The two publication gates

They do not substitute for each other, and each is built only from its own
table, so neither can be filled in from the other's evidence:

- **UI model replay** gates the RuntimeModel: one `frame` and one `transition`
  trusted replay result over the pinned corpora, recorded with
  `trusted record-replay`.
- **Project/operation replay** gates the ProjectPlugin: one passing replay of
  one Replay Bundle, recorded with `trusted record-project-replay`.

`publish` therefore takes `--frame-result-id`, `--transition-result-id` and
`--project-operation-result-id`. A gate that is absent, malformed, already
consumed, bound to another candidate revision, or backed by a bundle whose
frame retention has run out fails the publication closed; nothing is exported.
Both halves are inlined into one `ReplayGate` document whose hash is the
release manifest's `replay_gate_hash`.

Publication compiles canonical `page-model.toml`, closes over every referenced
asset, builds the two-gate `ReplayGate`, stages a RuntimeArtifact, then
performs one short SQLite transaction. That transaction checks the candidate
head and predecessor, inserts the two UI replay attestations, the
project/operation attestation and the publication, and advances
`published_head`. Only after commit is the handoff directory made visible.
Startup recovery deletes stale staging/unreferenced objects and recreates any
committed handoff interrupted after the database commit.

The RuntimeArtifact manifest has only the v1 fields
`manifest_schema_hash`, `runtime_model_schema_hash`, `page_model`, and
`assets`, encoded as exact RFC 8785 JCS bytes.
