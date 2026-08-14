# Hash management simplification proposal — 2026-08-14

> **Status: proposal, not current execution authority.** This document records
> a candidate simplification against repository commit `dc109bd`. It does not
> change the current bundle, schema, ProjectRegistration, SessionManifest,
> Operator or trace contracts. Approval must first amend those authorities and
> place any implementation work in the consumer repository's one canonical
> execution plan.

## Decision requested

Adopt this repository rule:

> Developers author no hash values. Exact content identity is retained only at
> real immutable-byte boundaries, is generated automatically, and is hidden
> behind a small number of aggregate release or blob identities.

The target is not "no SHA-256 implementation". The target is:

- no hand-maintained digest in C++, Luau, Python, Markdown or project input;
- no exact document digest used as a compatibility version;
- no duplicate digest copied across authorities;
- no new hash for HostPlugin composition;
- a few automatically minted aggregate identities where exact bytes really
  cross a trust, persistence or replay boundary.

## Measured baseline

The current tree has seven JSON Schemas containing hash fields. A source scan
at `dc109bd` finds:

- 52 distinct hash-shaped schema field names;
- 108 hash-field occurrences in `umbraflow-operator-v1.schema.json`;
- 57 in `umbraflow-annotation-workspace-v2.schema.json`;
- 84 production or schema files mentioning hash identity;
- 37 test files doing the same;
- one consumer specification bundle root copied into the verifier and four
  live documents.

Not all of this is developer friction. Many Operator hashes are calculated at
runtime and never authored by a person. The problem is that four different
jobs currently share the same `ContentHash` vocabulary:

| Job | Example | Actual requirement |
|---|---|---|
| compatibility selection | consumer spec bundle and framework schema hashes | compatible format or accepted contract version |
| immutable release verification | ProjectPlugin bytes and RuntimeArtifact closure | exact bytes |
| persistent authority binding | ProjectRegistration and SessionManifest | one immutable aggregate identity |
| internal audit/idempotency | plan, intent, state and evidence hashes | deterministic runtime fact, not developer input |

Using exact document bytes for the first job is the source of most avoidable
churn. Compatibility is semantic; a digest can report only equality.

## Hash admission rule

A new or retained hash field must satisfy all four conditions:

1. immutable bytes cross a process, trust, persistence or replay boundary;
2. exact byte equality, rather than compatible interpretation, is required;
3. the authoritative producer calculates the value automatically;
4. a named consumer recomputes or resolves it and refuses a concrete mismatch.

If any condition is false, use a format version, stable id, database revision,
held byte owner or ordinary structured data instead.

A hash must never be introduced merely because data "may be useful for audit"
or because two components need a version. The proposal adding the field must
name the producer, verifier, refusal and attack or recovery failure it closes.

## Remove first

### Cross-repository specification bundle pin

`scripts/check_spec_bundle.py` hard-codes one exact root. The same root is
repeated in `ARCHITECTURE.md`, `INDEX.md`, the runtime hardening authority and
the migration report. It has already moved repeatedly as legitimate design
documents changed.

This pin uses byte equality as cross-repository change control. Remove it from
the normal repository and development gates.

Replace it with:

- an explicit consumer contract/release version;
- schema and conformance checks over the current selected consumer checkout;
- a release-only record of the tested consumer revision when producing a
  release report.

The release record is evidence after a test run. It is not a checked-in digest
that blocks the next edit.

### Hand-written framework schema digests

The following are current manual copies of schema or database identity:

- `task::k_runtimeArtifactSchemaHash`;
- `task::k_runtimeModelSchemaHash`;
- Luau `model.schema_hash`;
- `trace::k_traceSchemaHash`;
- `detail::k_annotationWorkspaceSchemaHash`;
- `detail::k_workspaceSqliteSchemaHash`.

`tests/test-runtime-surface.py` exists partly to keep these copies synchronized.
The repository also already generates `FrameworkSchemaCatalog` from the exact
files under `schema/`, so a second hand-authored digest is unnecessary where a
runtime consumer truly needs those bytes.

Replace fixed schema digests with named format versions:

```text
runtime_artifact_format = 1
runtime_model_format = 2
annotation_workspace_format = 2
trace_format = 2
```

The trusted parser and validator decide whether that format is supported.
Cross-language compatibility is proved by conformance cases, not by C++, Luau
and Python copying the same digest.

### HostPlugin composition identity

Do not add `runtime_composition_hash`. A HostProfile and active HostPlugin list
remain readable structured diagnostics. An immutable release may record the
executable revision that was tested, but no composition digest becomes an
admission token or a value developers maintain.

## Collapse behind aggregate identities

### Project release

Project authors provide plugin source, schemas, catalogs and assets without
hash fields. The Project Kit publishes one immutable release manifest and
calculates its identity.

Leaf digests may remain inside that generated manifest to verify its file
closure. They are build output, not project authoring input and not copied into
unrelated records.

The public runtime boundary carries one aggregate Project release or verified
registration identity. Schema owners obtain the already verified bytes or
resource handles from that release instead of asking callers to repeat each
schema hash.

### Runtime artifact

Keep one automatically generated RuntimeArtifact identity and the internal
file-closure verification. Remove manifest fields whose only purpose is to
repeat the exact digest of a fixed framework schema. A format version selects
the parser; the parser validates the content.

### Session

A Session record continues to name the actual Project release, RuntimeArtifact,
policy and Agent profile it uses. Fixed Host schema digests do not belong in
every SessionManifest.

Whether the resulting immutable SessionManifest keeps an automatically derived
root or is addressed only by `session_id` is a later protocol decision. The
first simplification does not need to rewrite every Operator join to answer it.

## Retain internally for now

The first simplification keeps these automatically produced checks:

- ProjectPlugin exact bytes equal the verified Project release;
- Project-owned artifact blobs equal the generated release closure;
- RuntimeArtifact files equal the installed immutable release;
- evidence blobs remain content-addressed where the blob store uses the digest
  as its retrieval and integrity identity;
- trace events remain attached to one Session identity;
- Operator plan, intent, state and evidence hashes remain where they currently
  provide idempotency, recovery or portable audit evidence.

These values impose little authoring cost. Removing them together would rewrite
the Operator wire schema, SQLite records, conformance suite and replay model.
They should be audited later for duplicate carriage, but not mixed into the
developer-friction repair.

## Development and release behavior

```text
Development
  edit source/schema/config
  -> build and conformance validate current content
  -> no hash file is edited or accepted as input

Publish
  validated source tree
  -> publisher writes immutable manifest and internal leaf digests
  -> publisher returns one aggregate release identity

Runtime
  release identity plus held/verifiable bytes
  -> Host verifies exact immutable boundaries
  -> normal execution uses typed owners and stable ids
```

A test fixture should construct the same release through a fixture builder. It
must not reproduce production JSON by formatting a page of digest fields.

## Options and estimates

Estimates assume one senior engineer familiar with both repositories and
include schema, fixture, documentation and CI changes.

| Option | Outcome | Estimate |
|---|---|---:|
| A. Development-friction repair | remove cross-repository root pin and all hand-authored schema digest copies | 11–21 person-days |
| B. Aggregate public identities | A plus collapse ProjectRegistration/RuntimeArtifact/Session duplicate leaf identities | 26–51 person-days total |
| C. Remove runtime content identity | rewrite Operator, trace, replay and persistence without content hashes | 60–120+ person-days |

Recommendation: approve A, evaluate B field by field under the admission rule,
and reject C as a repository-wide objective.

## Delivery stages

### Stage 0 — contract inventory

- classify every live hash field by producer, consumer and refusal;
- mark hand-authored values and duplicate copies;
- identify which consumer contract rows must change before implementation;
- freeze no new hash-bearing schema during the inventory.

Estimate: 1–2 person-days.

### Stage 1 — remove the external bundle root

- remove the root constant and exact-byte normal development gate;
- replace it with selected-version plus semantic/conformance verification;
- keep tested consumer revision in generated release evidence only;
- remove duplicate root prose from live documents.

Estimate: 3–6 person-days across both repositories.

### Stage 2 — replace fixed schema pins

- add explicit format versions to the affected documents;
- select trusted parsers/validators by format version;
- use generated schema catalog bytes where exact schema validation remains;
- delete C++/Luau/Python digest constants and the synchronization gate;
- convert fixtures to builders rather than hand-formatted hash documents.

Estimate: 7–13 person-days.

### Stage 3 — optional aggregate-boundary cleanup

- remove repeated leaf schema hashes from public runtime records;
- bind verified byte/resource owners to the aggregate Project release;
- remove fixed Host schema hashes from SessionManifest;
- retain internal leaf verification in generated immutable manifests.

Estimate: 15–30 additional person-days.

## Acceptance

1. Editing a framework schema requires no manual digest edit.
2. Editing a compatible consumer design document does not fail framework CI
   because its bytes changed.
3. Development inputs contain no SHA-256 values that a user must calculate.
4. A ProjectPlugin whose bytes differ from its published release is still
   refused.
5. A RuntimeArtifact whose file closure differs from its published release is
   still refused.
6. Format-version mismatches fail with the supported and supplied versions,
   not two opaque digests.
7. Publish output is deterministic and calculates all retained leaf digests.
8. The standard CLI and conformance fixtures do not hand-format fixed schema
   hashes.
9. No HostPlugin profile or descriptor contains a checked-in composition hash.
10. The full repository gates and the real consumer conformance run remain
    green.

## Stop conditions

Keep an internal digest when removing it would:

- reopen a check-then-execute byte substitution path;
- make an immutable artifact closure unverifiable;
- make an external blob impossible to identify or verify;
- replace deterministic replay evidence with process-local pointer identity;
- require accepting multiple formats without an explicit parser/migration
  decision.

Stopping at such a boundary is not failure. The objective is to remove hash
management, not to prohibit hashing.

## Approval consequences

Approval requires coordinated amendments rather than deleting pins in place:

1. change the frozen bundle authority and its consumer counterpart;
2. amend the affected JSON Schemas before changing producers or parsers;
3. update `ARCHITECTURE.md` to state the hash admission rule;
4. place the accepted stages in the consumer repository's canonical execution
   plan;
5. retain this document as rationale and measurement, not a second work list.

Until those amendments land, current exact-byte and pin checks remain
authoritative.
