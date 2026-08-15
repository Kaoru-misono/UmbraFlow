# Framework hash cleanup — measurement and staging, 2026-08-14

> **Archived 2026-08-15. Nothing it owes remains only here.** All five stages
> H1–H5 are implemented in the framework repository; §4's execution note lists
> what no longer exists. The one thing that survived this document is the
> consumer-side repair described in [§8](#8-named-consumer-side-consequences),
> and it is now owned by `CH-01a` in the consumer repository's
> `docs/architecture/parallel-implementation-plan.md` — the six sites and the
> member-ordering trap are stated there. The specification bundle pin, excluded
> throughout, stays owned by
> [the hash management proposal](../../2026-08-14-hash-management-simplification-proposal.md),
> which is not archived. The measurements and classifications below are read as
> the record of what was found in August 2026, not as the current tree.

> **Status: rationale, measurement and staging. Not an unfinished-work list.**
>
> The consumer repository's `docs/architecture/parallel-implementation-plan.md`
> is the only canonical owner of unfinished work. Nothing in this document
> becomes real work until the rows in [§6](#6-rows-to-lift-into-the-consumer-plan)
> are lifted into that plan's `§3` U-series sub-package tables. This document
> may explain a row; it does not open a second copy of one. Two items measured
> here already have owners there and are deliberately **not** given rows:
> Operator DDL identity (`U2a`) and `decision_basis_hash` having no reader
> (`U12d` item 3).
>
> Companion rationale: [hash management simplification proposal](../2026-08-14-hash-management-simplification-proposal.md).
> That proposal's baseline was commit `dc109bd`. Every number below was
> re-measured at the current tree (HEAD `f9121d6` plus 22 uncommitted files);
> where the two disagree, [§7](#7-where-the-proposal-is-inaccurate) says which
> is wrong.

Measurement conditions: `modules/operator/**`, `modules/deployment/**` and
`tests/operator/**` were mid-edit by another lane while these counts were taken.
Nothing in those paths was modified here, and every count below is reproducible
from the stated command.

## 1. Scope

**In scope: the framework repository only** — `modules/`, `entry/`, `schema/`,
`tests/`, `tools/`, `scripts/`, `cmake/`.

**Out of scope, stated rather than silently omitted:**

1. **The cross-repository specification bundle pin.** `scripts/check_spec_bundle.py:41`
   holds root `ac8c3fa6…`, repeated verbatim in five live documents
   (`docs/ARCHITECTURE.md:18`, `docs/INDEX.md:79`, `docs/TODO.md:325`,
   `docs/plans/2026-08-09-runtime-hardening-rewrite.md:14`,
   `docs/plans/2026-08-09-runtime-migration-report.md:8`), and in the consumer's
   `docs/architecture/spec-bundle.manifest.json` and `conformance/interface-lock/**`.
   It is a two-repository decision the owner has deferred. One factual note for
   whoever picks it up: `check-spec-bundle` **is** labelled `CI`
   (`tests/CMakeLists.txt:1155-1168`) and therefore runs inside
   `ctest -L CI`, i.e. inside `scripts/ci-local.ps1:43`. The consumer plan's
   line 48 claim that the gate "不进 `ci-local`" is stale. The proposal is right
   that the pin sits in the normal development gate; the consumer plan is wrong.
2. **Project-side and consumer-side hash authoring.** `project_state_schema_hash`,
   `project_observation_schema_hash`, `project_tool_precondition_schema_hash`,
   `payload_schema_hash`, `tool_catalog_hash` and `effect_payload_sha256s` are
   authored by a project (uf-chaos `modules/project_state/registration.py:109-133`).
   The framework defines their shape; it does not author their values. Excluded.
3. **The proposal's option C** (removing runtime content identity). Recommended
   for rejection there; not planned here.

Where a framework-side change forces a consumer-side change, it is named in
[§8](#8-named-consumer-side-consequences) and stops there.

## 2. Measured inventory

### 2.1 Hand-authored 64-hex literals in production sources

```bash
rg -c -N --no-heading -g '!**/external/**' '\b[0-9a-f]{64}\b' modules/ entry/
```

**15 literals across 5 files.** No literal in this repository is split across
adjacent string literals; the following returns nothing outside the pixel
fixture named in §2.2:

```bash
rg -U --multiline-dotall -g '!**/external/**' -g '!tests/vision/label-fixture.hpp' \
   '"[0-9a-f]{8,63}"\s*\n\s*"[0-9a-f]{1,63}"' modules/ entry/ tests/ tools/ schema/ examples/
```

| # | Site | Name | Produced by | Verified by | Refusal on mismatch | Job |
|---|---|---|---|---|---|---|
| 1 | `modules/trace/source/trace/event.hpp:18` | `uf::trace::k_traceSchemaHash` | a human, copied from `schema/umbraflow-trace-v2.schema.json` bytes | `tests/test-runtime-surface.py:227-232` only, against the file | none at runtime | compatibility (claimed); none in fact |
| 2 | `modules/task/source/task/runtime-model-file.hpp:30` | `uf::task::k_runtimeArtifactSchemaHash` | a human | `tests/test-runtime-surface.py:238-242` + `modules/task/source/task/runtime-model-file.cpp:772-779` | "runtime artifact manifest schema is not supported by this Host: the manifest states … and this Host accepts …" | compatibility |
| 3 | `modules/task/source/task/runtime-model-file.hpp:33` | `uf::task::k_runtimeModelSchemaHash` | a human | `tests/test-runtime-surface.py:243-247` + `runtime-model-file.cpp:781-788` | "runtime model schema is not supported by this trusted parser: …" | compatibility |
| 4 | `modules/task/runtime/model.luau:10` | `model.schema_hash` | a human — a **third** copy of #3 | `tests/test-runtime-surface.py:233-237`; at runtime `modules/task/source/task/task-host.cpp:474-479` | "trusted Runtime parser schema does not match the artifact" | compatibility |
| 5 | `modules/operator/source/operator/runtime-installation.hpp:20` | `uf::operator_runtime::detail::k_annotationWorkspaceSchemaHash` | a human | `tests/test-runtime-surface.py:248-252` + `runtime-installation.cpp:244-254` | "release manifest uses an unsupported annotation schema" | compatibility |
| 6 | `modules/operator/source/operator/runtime-installation.hpp:31` | `uf::operator_runtime::detail::k_workspaceSqliteSchemaHash` | a human, copied from Python `tools/annotate/store.py:485` | **nothing in this repository** — see §7 | `runtime-installation.cpp:277-287`, "release manifest uses an unsupported workspace schema" | compatibility |
| 7 | `modules/operator/source/operator/ledger.cpp:478` | `k_operatorDatabaseSchemaIdentity` | a human, recomputed from a freshly created database | `ledger.cpp:627-633`, run immediately after schema creation | schema-identity refusal | persistent authority binding |
| 8–15 | `modules/operator/source/operator/ledger.cpp:834, 840, 846, 852, 858, 864, 870, 876` | eight `SchemaMigration::sourceIdentity` values | frozen history — the identity of an Operator database already on disk | `ledger.cpp:883-913` | "Operator database schema identity … has no registered audit-preserving disposition to …; database left intact" | persistent authority binding |

### 2.2 Hand-authored 64-hex literals in tests and tools

```bash
rg -c -N --no-heading -g '!**/external/**' '\b[0-9a-f]{64}\b' tests/ tools/
```

Raw total 1309. **1296 of them are `tests/vision/label-fixture.hpp` gray8 pixel
data, not digests** (`tests/vision/label-fixture.hpp:7-13` states the encoding);
excluding that file gives **12 in `tests/` and 1 in `tools/`**. Of the 13, five
are synthetic placeholders that no edit can invalidate and eight are real
digests:

| Site | What it is | Class |
|---|---|---|
| `tests/deployment/test-project-deployment.cpp:1265` | exact sha256 of `schema/umbraflow-collection-fact-v1.schema.json`, asserted against the generated catalog | real digest, hand-authored |
| `tests/deployment/test-project-deployment.cpp:1298` | same, `umbraflow-fact-provenance-v1.schema.json` | real digest, hand-authored |
| `tests/deployment/test-project-deployment.cpp:1309` | same, `umbraflow-fact-v1.schema.json` | real digest, hand-authored |
| `tests/operator/test-ledger.cpp:2187, 2212, 2359` | past Operator DDL identities, compared against an identity the test reconstructs | real digest, golden anchor |
| `tests/script/test-pure-data-boundary.cpp:440` | the plugin-environment material digest | real digest, review tripwire |
| `tools/annotate/tests/test_backend.py:870` | the Python `SCHEMA_ROOT_HASH`, i.e. the Python copy of #6 | real digest, hand-authored |
| `tests/deployment/test-project-deployment.cpp:1509` | `…00a1` | placeholder |
| `tests/engine/test-session.cpp:660` | `1111…` | placeholder |
| `tests/project/test-authoring-path-parity.cpp:53` | `0000…` | placeholder |
| `tests/project/test-project-kit.cpp:1815` | `0000…` | placeholder |
| `tests/task/test-runtime-model-v2.luau:191` | `aaaa…` in a rejection case | placeholder |

### 2.3 Hash fields in every document under `schema/`

```bash
for f in schema/*.json; do
  echo "$f $(rg -o -N --no-heading '"[A-Za-z0-9_]*(?:hash|digest|sha256)[A-Za-z0-9_]*"' "$f" | wc -l)"
done
```

**13 schema documents; 7 contain hash-shaped fields; 225 occurrences; 53
distinct names.**

| Document | Occurrences | Distinct names |
|---|---:|---:|
| `umbraflow-operator-v1.schema.json` | 110 | 24 |
| `umbraflow-annotation-workspace-v2.schema.json` | 62 | 17 |
| `umbraflow-project-registration-v1.schema.json` | 19 | 10 |
| `umbraflow-journal-v1.schema.json` | 18 | 7 |
| `umbraflow-runtime-artifact-v1.schema.json` | 9 | 3 |
| `umbraflow-trace-v2.schema.json` | 5 | 3 |
| `umbraflow-policy-v1.schema.json` | 2 | 1 |
| `umbraflow-collection-fact-v1.schema.json` | 0 | 0 |
| `umbraflow-declarative-workflow-tool-v1.schema.json` | 0 | 0 |
| `umbraflow-fact-provenance-v1.schema.json` | 0 | 0 |
| `umbraflow-fact-v1.schema.json` | 0 | 0 |
| `umbraflow-project-v1.schema.json` | 0 | 0 |
| `umbraflow-runtime-v2.schema.json` | 0 | 0 |

**The published schema surface has not moved since `dc109bd`.** The same command
run against `git show dc109bd:…` returns 110, 62 and 53 distinct names. What
moved is that two new schemas landed —
`schema/umbraflow-declarative-workflow-tool-v1.schema.json` and the still
untracked `schema/umbraflow-project-v1.schema.json` — and **both carry zero hash
fields.** Their only `hash` substrings are prose inside `$comment`
(`umbraflow-project-v1.schema.json:99`,
`umbraflow-declarative-workflow-tool-v1.schema.json:28`). New framework schema
work already meets the admission rule without being told to.

Of the 53 distinct names, 13 are fixed framework or project schema digests and
40 are runtime-computed audit, idempotency, closure or content-addressing
values. Only the framework-owned members of the first group are in scope.

Two name collisions in that group matter for staging, because a reader who
treats each name as one field will scope a change wrongly:

- **`manifest_schema_hash` is two unrelated fields.** One is the RuntimeArtifact
  manifest's (`schema/umbraflow-runtime-artifact-v1.schema.json:15`), pinned to a
  compiled-in constant; the other is the ProjectRegistration manifest's
  (`schema/umbraflow-project-registration-v1.schema.json:21`), derived at
  `modules/deployment/source/deployment/project-directory.cpp:1117-1120` from the
  build-generated catalog bytes and refused at
  `modules/operator/source/operator/manifest.cpp:109-121` with "ProjectRegistration
  was validated against a different schema root". Only the first is in scope.
- **`runtime_model_schema_hash` has a second, dead declaration.** Besides the
  RuntimeArtifact manifest it appears in `$defs.RuntimeModelBindingRef`
  (`schema/umbraflow-operator-v1.schema.json:85-101`). That definition is
  `$ref`-ed by nothing in `schema/`, and `RuntimeModelBindingRef` has zero
  matches anywhere in `modules/`, `tests/` or `entry/`. It is an orphan.

  > **Corrected 2026-08-15, before implementing Stage H5.** Not being `$ref`-ed
  > is *not* what makes it an orphan, and the original wording invited the wrong
  > guard. Measured at HEAD `7d5ba0f`: the Operator protocol schema declares
  > **50** `$defs`; **38** are the target of a `#/$defs/<name>` reference
  > somewhere under `schema/`; **12 are `$ref`-ed by nothing** — `AgentBudget`,
  > `ApprovalToken`, `ControllerCapability`, `DecisionBasis`,
  > `ExternalInputFinding`, `PlanProposal`, `ProgressMarker`, `ProjectSnapshot`,
  > `ResyncRequired`, `RuntimeModelBindingRef`, `SessionManifest` and
  > `SnapshotParts`. Eleven of those twelve are top-level protocol shapes that
  > contract tests and runtime code name by **bare definition name**, which is
  > exactly why nothing `$ref`s them. Counting the `.c/.cc/.cpp/.h/.hpp/.luau`
  > files under `modules/`, `tests/` and `entry/` that name each one outside a
  > comment — the reading `definition_consumer_errors` in
  > `tests/test-runtime-surface.py` performs — gives `SessionManifest` 20,
  > `AgentBudget`, `PlanProposal` and `ResyncRequired` 4 each, `ApprovalToken`
  > 3, `ExternalInputFinding` and `SnapshotParts` 2 each,
  > `ControllerCapability`, `DecisionBasis`, `ProgressMarker` and
  > `ProjectSnapshot` 1 each — and `RuntimeModelBindingRef` **0**. The property
  > that isolates it is **having a consumer**, not being referenced. See
  > [Stage H5](#stage-h5--delete-the-two-hash-carriers-nothing-reads).

### 2.4 File-level breadth

```bash
rg -l -N -g '!**/external/**' '(?:ContentHash|_hash|Hash\b)' modules/ entry/ schema/     # 85
rg -l -N -g '!**/external/**' -g '!tests/vision/label-fixture.hpp' \
      '(?:ContentHash|_hash|Hash\b)' tests/                                              # 38
```

**85 production or schema files and 38 test files.** At `dc109bd` the same
commands give 82 and 37; the three added production files are
`modules/project/source/project/declarative-workflow-tool.cpp` and the two new
schemas, and in all three the match is prose, not a field.

### 2.5 What `tests/test-runtime-surface.py` keeps synchronized, and why

`SCHEMA_AUTHORITIES` (`tests/test-runtime-surface.py:227-253`) holds **five**
entries, checked by `schema_authority_errors` (`:1086-1114`), which recomputes
`hashlib.sha256` over the named schema file and compares it to the literal
parsed out of the source:

| Constant | Pinned to |
|---|---|
| `k_traceSchemaHash` | `schema/umbraflow-trace-v2.schema.json` |
| `model.schema_hash` (Luau) | `schema/umbraflow-runtime-v2.schema.json` |
| `k_runtimeArtifactSchemaHash` | `schema/umbraflow-runtime-artifact-v1.schema.json` |
| `k_runtimeModelSchemaHash` | `schema/umbraflow-runtime-v2.schema.json` |
| `k_annotationWorkspaceSchemaHash` | `schema/umbraflow-annotation-workspace-v2.schema.json` |

The rule exists because these are copies. Its own comment (`:218-226`) states
the reason the Luau entry is there: a stale `model.schema_hash` "refuses every
artifact at activation, in a lane no local gate exercises", and the gate printed
`PASS` with that pin stale before the entry was added. `check-repository-surface`
is registered at `tests/CMakeLists.txt:1137-1143` and currently passes.

`k_workspaceSqliteSchemaHash` is **not** in this table and cannot be: no
checked-in file in this repository is its preimage. `runtime-installation.hpp:24-30`
says so at the declaration.

### 2.6 The generated catalog that already does this correctly

`scripts/embed_framework_schemas.py:1-8` reads every document under `schema/` at
build time and emits both the exact bytes and the sha256 of those bytes into
`framework-schema-catalog.generated.cpp` (`cmake/build.cmake:90`), surfaced as
`uf::framework_schema::FrameworkSchemaDocument` at
`modules/schema/source/schema/framework-schema-catalog.hpp:9-27`. Its digests
are recomputed on every build and cannot drift.

Module-graph note that constrains the repair: `modules/trace/manifest.txt`
declares `public = core domain` and `modules/task/manifest.txt` declares no
`schema` dependency at all. Routing the catalog into `trace` or `task` to feed
constants #1–#4 would mean adding module edges that `scripts/check_modules.py`
enforces, and in `trace`'s case would contradict the module's stated purpose.
The catalog is the right answer where exact schema **bytes** are needed for
validation; it is not the right answer for a compatibility pin.

## 3. Classification against the admission rule

The rule: a hash field is admissible only if **(1)** immutable bytes cross a
process/trust/persistence/replay boundary, **(2)** exact byte equality rather
than compatible interpretation is required, **(3)** the authoritative producer
calculates it automatically, and **(4)** a named consumer recomputes or resolves
it and refuses a concrete mismatch.

### 3.1 Removal candidates

| Item | 1 | 2 | 3 | 4 | Why it fails |
|---|:-:|:-:|:-:|:-:|---|
| `k_traceSchemaHash` and the trace envelope's `payload.schema_hash` | ✗ | ✗ | ✗ | ✗ | Fails all four. The slot is a **per-payload** schema identity (`modules/trace/source/trace/event.hpp:54`), and all four first-party emitters stamp the same envelope-schema constant into it: `modules/engine/source/engine/session.cpp:53-60, 98`, `modules/task/source/task/task-host.cpp:100-103, 148`, `modules/task/source/task/task-context.cpp:53-56, 97, 597`, `modules/task/source/task/exploration-session.cpp:39-42, 97`. It therefore distinguishes nothing. No first-party code compares it; the only verifier is the surface gate comparing the literal to the file. `k_traceSchema = "umbraflow-trace/v2"` (`event.hpp:17`) already rides in the same envelope at `event.cpp:161` and already names the format. |
| `k_runtimeArtifactSchemaHash`, `k_runtimeModelSchemaHash`, `model.schema_hash`, and the manifest fields `manifest_schema_hash` / `runtime_model_schema_hash` | ✓ | ✗ | ✗ | ✓ | Fails 2 and 3. What the Host wants is "this manifest is a shape my parser supports", which is compatibility, not byte identity — every cosmetic edit to a schema file breaks every published artifact. Fails 3 three times over: the same digest is typed by hand in C++ twice, in Luau once, and by the consumer's two generators. Producer-side is already automatic (`tools/annotate/publication.py:258, 260`); only the acceptors are hand-typed. |
| `k_annotationWorkspaceSchemaHash` and the release manifest's `annotation_workspace_schema_hash` | ✓ | ✗ | ✗ | ✓ | Same shape as above, one document further out. Producer `tools/annotate/publication.py:492` computes it; acceptor `runtime-installation.hpp:20` types it. |
| `k_workspaceSqliteSchemaHash` and `workspace_sqlite_schema_hash` | ✓ | ✗ | ✗ | ✓ | Fails 2 and 3. The value is `sha256` over a JCS document naming the workspace application id, `user_version` and every schema object's normalized SQL (`tools/annotate/store.py:485`). What the Host needs to know is "is this workspace database a generation I understand" — a database revision. Condition 3 fails twice: `runtime-installation.hpp:31` and `tools/annotate/tests/test_backend.py:870` are both hand transcriptions of a value only `store.py` computes. |
| `tests/deployment/test-project-deployment.cpp:1265, 1298, 1309` | ✓ | ✓ | ✗ | ✓ | Fails 3 alone. The catalog entries being asserted are generated correctly; the expectations are typed. Any byte edit to those three schemas turns the test red for no semantic reason, and the fix is a transcription. |
| `SessionManifest.journal_envelope_schema_hash` (`schema/umbraflow-operator-v1.schema.json:144, 152`) | ✓ | ✓ | ✓ | ✗ | Fails 4 alone, and that is enough. It is written at `modules/service/source/service/product-lifecycle.cpp:256-273` and serialized at `modules/operator/source/operator/manifest.cpp:95-96`, but `SessionManifest` declares **no getter for it** — `modules/operator/source/operator/manifest.hpp` publishes 13 hash accessors (`:95-176`) and this is not one of them. Nothing resolves it and nothing refuses a mismatch. Contrast `operator_protocol_schema_hash`, which sits in the same manifest and *is* refused at `modules/operator/source/operator/effective-plan.cpp:905`; that contrast is what makes this a defect rather than a style. |
| The orphan `$defs.RuntimeModelBindingRef` and its `runtime_model_schema_hash` / `runtime_model_artifact_root_hash` (`schema/umbraflow-operator-v1.schema.json:85-101`) | ✗ | ✗ | ✗ | ✗ | Fails all four by not existing anywhere else. No `$ref` reaches the definition **and** `RuntimeModelBindingRef` has zero matches in `modules/`, `tests/` or `entry/` — it is the conjunction that isolates it, since eleven other `$defs` are also unreferenced and every one of them is named by a source (see the correction note in §2.3). Two hash fields in the published Operator protocol that no producer writes and no consumer reads. |

### 3.2 Keepers — these stay, and the plan says so plainly

| Item | 1 | 2 | 3 | 4 | Why it stays |
|---|:-:|:-:|:-:|:-:|---|
| `RuntimeArtifact` file closure: `page_model.sha256`, `assets[].sha256`, the artifact root hash (`schema/umbraflow-runtime-artifact-v1.schema.json:26-56`, verified in `modules/task/source/task/runtime-model-file.cpp:760-764` and onward) | ✓ | ✓ | ✓ | ✓ | Immutable bytes cross a persistence and trust boundary; exact equality is exactly the property; `tools/annotate/publication.py:255` computes every leaf; `loadRuntimeArtifact` refuses a concrete mismatch. Untouched by every stage below. |
| `k_operatorDatabaseSchemaIdentity` and the eight `SchemaMigration::sourceIdentity` values (`modules/operator/source/operator/ledger.cpp:464-480, 831-878`) | ✓ | ✓ | ✗ | ✓ | Condition 3 fails literally — a human recomputes the target from a freshly created database — but `ledger.cpp:627-633` verifies immediately after schema creation, so a forgotten recomputation cannot ship green, and the eight source values name bytes already on disk that nothing can recompute. **Already owned by consumer-plan row `U2a`.** No row is opened here. |
| Vendored dependency pins: `modules/ocr/external/manifest.toml:33, 61, 73, 84, 93` and `modules/operator/external/manifest.toml:14`, verified by `scripts/fetch_external.py:129-130, 198-201` | ✓ | ✓ | ✗ | ✓ | The authoritative producer is upstream, not this repository; transcription happens once per dependency version bump and the download is refused on mismatch. This is supply-chain integrity, and the admission rule was never aimed at it. |
| `tests/script/test-pure-data-boundary.cpp:440`, the plugin-environment pin | ✓ | ✓ | ✗ | ✓ | Fails 3, and stays. The value never enters a production decision; it is the "external committed value" oracle that `docs/pitfalls/checks-that-cannot-fail.md` prescribes for "expected and actual come from one producer". Its preimage — bridge source, whitelist, frozen table surface, published contracts — is exactly what a framework upgrade must move deliberately (`tests/script/test-pure-data-boundary.cpp:410-413`). |
| `tests/operator/test-ledger.cpp:2187, 2212, 2359` | ✓ | ✓ | ✗ | ✓ | Golden anchors proving that the test's reconstruction of a prior DDL is faithful. Settled with `U2a`, not here. |
| ProjectRegistration's `manifest_schema_hash` (`schema/umbraflow-project-registration-v1.schema.json:21`) | ✓ | ✓ | ✓ | ✓ | The one fixed-schema digest in the family that already satisfies all four. Derived from the build-generated catalog at `modules/deployment/source/deployment/project-directory.cpp:1117-1120`, never typed, and refused at `modules/operator/source/operator/manifest.cpp:109-121`. It is the shape the others should have had. |
| `operator_protocol_schema_hash` (`schema/umbraflow-operator-v1.schema.json:149`, `schema/umbraflow-policy-v1.schema.json:126`) | ✓ | ✓ | ✓ | ✓ | Computed from the generated catalog at `modules/service/source/service/product-lifecycle.cpp:255-270`, refused twice: `modules/operator/source/operator/effective-plan.cpp:905` ("Operator protocol schema bytes do not match the pinned session manifest") and `modules/operator/source/operator/policy.cpp:240-294` ("PolicyArtifact answers for an operator protocol schema this session manifest does not pin"). No authoring cost, two named refusals. |
| The registration-pinned project schema digests — `project_state_schema_hash`, `project_observation_schema_hash`, `project_tool_precondition_schema_hash`, `journal_event_schema_manifest_hash`, `reconcile_payload_schema_manifest_hash` | ✓ | ✓ | ✓ | ✓ | All derived by `hashOf` over the project's real file bytes at `modules/deployment/source/deployment/project-directory.cpp:1228-1232` and refused by their owners (`project-plugin.cpp:246-255`, `journal-entry.cpp:296-320`, `reconcile-outcome.cpp:61-85`). Out of scope by §1.2 in any case, and they would pass the rule if they were in scope. One observation for whoever does own them: the first three share one refusal string that does not name which of the three mismatched. |
| The 40 runtime-computed Operator, journal and evidence hashes (plan, intent, state, evidence, capability, policy, registration, session and blob identities) | ✓ | ✓ | ✓ | mostly ✓ | Automatically produced, no authoring cost. The proposal's own "retain internally for now" section applies. The one measured condition-4 gap, `authority_decisions.decision_basis_hash` having no reader, is **already owned by consumer-plan row `U12d` item 3** and gets no row here. |

## 4. Stages

> **Execution status, 2026-08-15. Every stage H1–H5 is implemented in this
> repository.** Every measurement in [§2](#2-measured-inventory) and every
> classification in [§3](#3-classification-against-the-admission-rule) was taken
> before those stages landed and is read as the record of what was found, not as
> the current tree. None of the following exists any more: `k_traceSchemaHash`,
> `TypedTracePayload::schemaHash`, the trace envelope's `payload.schema_hash`,
> `SessionManifest.journal_envelope_schema_hash`, `$defs.RuntimeModelBindingRef`,
> `k_annotationWorkspaceSchemaHash`, `k_workspaceSqliteSchemaHash`, the Python
> `SCHEMA_ROOT_HASH`, `k_runtimeArtifactSchemaHash`, `k_runtimeModelSchemaHash`,
> `model.schema_hash`, `SCHEMA_AUTHORITIES` or `schema_authority_errors`.
>
> **No schema digest is pinned outside its schema file any more.** Editing a
> document under `schema/` moves no constant and reddens no gate, which was the
> acceptance criterion of the whole exercise. What remains in scope is the
> consumer's half of H3 ([§8](#8-named-consumer-side-consequences)) and the
> deferred specification bundle pin ([§1](#1-scope)).

Each stage is independently deliverable, leaves the tree green on its own, and
is ordered so that no stage depends on a later one. Each guard is stated with
the mutation that must turn it red and the assertion the red must land on, per
the five-step rule in `docs/pitfalls/checks-that-cannot-fail.md`.

### Stage H1 — delete the trace payload schema digest

**Changes.** Remove `k_traceSchemaHash` (`modules/trace/source/trace/event.hpp:18-20`)
and `TypedTracePayload::schemaHash` (`:54`); stop emitting `payload.schema_hash`
(`modules/trace/source/trace/event.cpp:178-179`); remove `schema_hash` from
`payload.required` and `payload.properties`
(`schema/umbraflow-trace-v2.schema.json:257-272`); drop the four emitter helpers
named in §3.1; drop the `k_traceSchemaHash` row from `SCHEMA_AUTHORITIES`
(`tests/test-runtime-surface.py:229-232`).

**What replaces it.** Nothing new. `k_traceSchema = "umbraflow-trace/v2"`
(`event.hpp:17`) is already the format version and is already emitted at
`event.cpp:161`.

**Guard.** A trace-envelope test asserting (a) the emitted envelope carries
`"schema":"umbraflow-trace/v2"` and (b) the emitted payload object has exactly
the members `fields` — no digest of any kind — plus the existing schema
validation of a recorded stream.

**Falsification.** Change `k_traceSchema` to `"umbraflow-trace/v3"`: the red must
land on assertion (a) in `tests/trace/test-trace.cpp`, not on a JSON parse error
or on the stream validator's session check. Separately, re-add any `schema_hash`
member to the emitted payload: the red must land on assertion (b). If the second
mutation only reddens JSON-schema validation, assertion (b) is not doing its job
and the stage is not done.

**Estimate: 1–2 person-days.** Assumes one engineer, the four emitter sites and
the two trace tests are the whole blast radius (measured: 6 references in 6
files), and no consumer reads the trace stream — confirmed, uf-chaos contains no
occurrence of `umbraflow-trace`.

### Stage H2 — replace the workspace pair with a workspace revision

**Changes.** Replace `annotation_workspace_schema_hash` and
`workspace_sqlite_schema_hash` in the release manifest with
`annotation_workspace_format` and `workspace_sqlite_revision` integers; delete
`k_annotationWorkspaceSchemaHash` and `k_workspaceSqliteSchemaHash`
(`modules/operator/source/operator/runtime-installation.hpp:20-33`) and rewrite
the two comparisons in `runtime-installation.cpp:244-254, 277-287` to compare
integers and name both numbers in the refusal; change the producer
(`tools/annotate/publication.py:492, 499`) and both Python verifiers
(`tools/annotate/store.py:485, 2250-2258`, refusal "release manifest authority
inputs are inconsistent"); delete the `SCHEMA_ROOT_HASH` assertion at
`tools/annotate/tests/test_backend.py:870`; drop the
`k_annotationWorkspaceSchemaHash` row from `SCHEMA_AUTHORITIES`; update the
fixtures at `tests/support/umbraflow/project-fixture.hpp:158-161`,
`tests/operator/test-ledger.cpp:771-774, 2970-2997` and
`modules/conformance/source/conformance/observation-fixture.hpp:179-182`.

Note the manifest is parsed **positionally** against exact JCS bytes
(`runtime-installation.cpp:247`), so field renames are a hard break in the
reader and in every recorded manifest — which is the intended shape of the
change, not a reason to add a second accepted spelling.

**What replaces it.** Two integers selected by the trusted parser.

**Guard.** Two cases: an unsupported `workspace_sqlite_revision` is refused with
a message naming the supported and the supplied revision; and a **positive
control** — editing a comment inside the workspace DDL in `store.py`, which
today moves `SCHEMA_ROOT_HASH` and breaks the handoff, must leave the whole gate
green.

**Falsification.** Publish a release manifest at revision `supported + 1`: the
red must land on the revision comparison in `parseReleaseManifest`, and the
message must contain both numbers. Then delete the comparison entirely: the same
case must go red for the opposite reason, proving the case reaches that branch
rather than being refused earlier by the positional reader. The positive control
is falsified by reverting Stage H2 and confirming the comment edit is red again.

**Estimate: 3–5 person-days.** Assumes the release manifest reader stays
positional, that the revision numbering is a single monotonic integer chosen by
`store.py`, and that no released workspace database exists that must be
migrated. **No consumer-side work**: `annotation_workspace_schema_hash` and
`workspace_sqlite_schema_hash` appear nowhere in uf-chaos.

> **Recorded 2026-08-15, while implementing this stage.** Three decisions the
> Changes list above left open, so the next reader does not re-derive them.
>
> 1. **Both numbers already existed; neither was invented.**
>    `workspace_sqlite_revision` **is** `store.py`'s `SCHEMA_VERSION`, the
>    SQLite `user_version` the package already writes and already refuses a
>    database for (`store.py`'s `PRAGMA user_version` check). Minting a second
>    monotonic integer beside it would have been two spellings of one fact.
>    `annotation_workspace_format` is a new `ANNOTATION_WORKSPACE_FORMAT = 2`,
>    the `v2` the contract already names in its own `$id`.
> 2. **`SCHEMA_ROOT_HASH` is deleted outright, not left as a diagnostic.** The
>    Changes list named `store.py:485` — its definition — without saying what
>    became of it, and it had two emission sites the list did not mention:
>    `serve.py`'s `schema_manifest()` (`sqlite_schema_root_hash`) and
>    `trusted.py`'s init report (`schema_root_hash`). Once the release manifest
>    stops carrying it, nothing refuses a mismatch anywhere, which is exactly
>    the "carrier nothing reads" [Stage H5](#stage-h5--delete-the-two-hash-carriers-nothing-reads)
>    removed. Both emissions already sat beside `user_version`/`SCHEMA_VERSION`,
>    so no reader lost a fact. `ANNOTATION_CONTRACT_HASH` went with it.
> 3. **The guard's Python half is not Python compared against Python.** The
>    deleted `test_backend.py` assertion said so of itself. Its replacement
>    compares the published `workspace_sqlite_revision` against the
>    `PRAGMA user_version` the exported database actually carries, and
>    `annotation_workspace_format` against the generation parsed out of the
>    contract's `$id` — so a contract bumped to `v3` without bumping
>    `ANNOTATION_WORKSPACE_FORMAT` is red.

### Stage H3 — replace the runtime-artifact pair with format versions

**Scope note.** This stage touches the **RuntimeArtifact** `manifest_schema_hash`
only. The identically named ProjectRegistration field
(`schema/umbraflow-project-registration-v1.schema.json:21`) is a keeper and is
not to be edited; see §2.3 and §3.2.

**Changes.** Replace `manifest_schema_hash` and `runtime_model_schema_hash` in
the RuntimeArtifact manifest with `runtime_artifact_format` and
`runtime_model_format` integers
(`schema/umbraflow-runtime-artifact-v1.schema.json:8-19`); delete
`k_runtimeArtifactSchemaHash` and `k_runtimeModelSchemaHash`
(`modules/task/source/task/runtime-model-file.hpp:30-35`) and rewrite the two
refusals at `runtime-model-file.cpp:766-788` to name the supported and supplied
format; change the positional reader at `runtime-model-file.cpp:356-361`; replace
`model.schema_hash` (`modules/task/runtime/model.luau:10`) with
`model.format` and change what `project.luau:315` hands to
`runtime_model_finalize`, `modules/task/source/task/ffi/uf-tables.cpp:1520-1530`
and the comparison at `task-host.cpp:474-479`; regenerate
`examples/umbraflow/runtime/artifact/runtime-artifact.manifest.json` and
`examples/arcana-expedition/runtime/artifact/runtime-artifact.manifest.json`;
change `tools/annotate/publication.py:204-206, 258-260`; drop the remaining
three `SCHEMA_AUTHORITIES` rows and, with the table now empty, delete
`schema_authority_errors` and its call site
(`tests/test-runtime-surface.py:1086-1114, 1316`) — a rule with an empty table
is a check that cannot fail. Update the fixtures at
`tests/support/runtime-v2-fixture.hpp:321-323`,
`tests/support/umbraflow/project-fixture.hpp:130-132`,
`tests/task/test-task-binding.cpp:95-97`,
`tests/task/test-adversarial-surface.cpp:76-79`,
`tests/operator/test-ledger.cpp:713-758`,
`tests/cli/test-open-project.cpp:148-149, 283` and
`modules/cli/source/cli/open-project.hpp:54`.

**What replaces it.** Two integers, plus a Luau `model.format` the Host compares
against the artifact's declared format.

**Guard.** Three cases: an artifact declaring an unsupported
`runtime_model_format` is refused with a message naming both numbers; a Luau
`model.format` that disagrees with the artifact is refused at
`finalizeRuntimeModel`; and the existing file-closure refusal still fires when
one asset byte is mutated.

**Falsification.** Build a fixture artifact with `runtime_model_format = 99`:
the red must land on the format comparison in `loadRuntimeArtifact` and the
message must contain `99` and the supported number. Set `model.format` to a
value the artifact does not declare: the red must land at
`task-host.cpp:474-479`, not earlier in the parse — this is the lane the surface
gate's own comment says no local gate exercised, so it must be reached
deliberately. Mutate one byte of an asset file: the red must remain the closure
refusal, proving the closure digests survived the stage. Finally, edit one
cosmetic byte of `schema/umbraflow-runtime-v2.schema.json`: the whole gate must
stay green. That is the acceptance criterion of the entire exercise, and it is
the case that is red today.

**Estimate: 4–7 person-days for the framework half.** Assumes the positional
readers stay positional, formats are single integers, and the two example
artifacts are regenerated by the existing publisher rather than edited. The
consumer half is not planned here; see [§8](#8-named-consumer-side-consequences).

> **Corrected and recorded 2026-08-15, while implementing this stage.** Four
> things the stage as written got wrong or left open.
>
> 1. **The guard's second case cannot be reached by publishing anything, and
>    the stage did not say so.** "A Luau `model.format` that disagrees with the
>    artifact is refused at `finalizeRuntimeModel`" reads as though a fixture
>    artifact could produce it. It cannot: `loadRuntimeArtifact` has already
>    held the artifact against `k_runtimeModelFormat` before finalize runs, so
>    an artifact that would disagree with the parser is refused one step
>    earlier. The refusal fires only for a binary built out of two halves —
>    `model.luau` reading one generation and `runtime-model-file.hpp` expecting
>    another. A case that cannot be written is a check that cannot fail, so the
>    seam is opened deliberately through
>    `TaskHostTestAccess::finalizeWithParserFormat`
>    (`modules/conformance/source/conformance/host-delivery-fixture.hpp`), and
>    the stage's own mutation was run as well: setting `model.format = 3` reddens
>    `contract-runtime-u01` on the finalize refusal, not on the parse.
> 2. **The two example artifacts have no publisher to regenerate them.**
>    `examples/umbraflow/runtime/artifact/runtime-artifact.manifest.json` and its
>    arcana sibling are checked-in JCS bytes that no script in this repository
>    produces — `tools/annotate/publication.py` writes into a workspace handoff,
>    never into `examples/`. They were rewritten by a one-off JCS re-encode in
>    the same change. Nothing records their root hashes, so both moved freely
>    (`d62dd622…`→`deeb8cf1…`, `8fe61aeb…`→`8a3c6107…`).
> 3. **The JCS member order changes, and the positional reader with it.**
>    `manifest_schema_hash` sorted before `page_model` and
>    `runtime_model_schema_hash` after it; both replacements sort after it, so
>    the reader is now `assets`, `page_model`, `runtime_artifact_format`,
>    `runtime_model_format`.
> 4. **Three neighbours came with the rename.** `ManifestReader::size` became
>    `unsignedInteger` because it now also reads a generation;
>    `TrustedRuntimeFinalize::parserSchemaHash` became `parserFormat`; and
>    `RuntimeModelBinding::runtimeModelSchemaHash()` was **deleted** rather than
>    renamed — it had zero call sites in `modules/`, `tests/` and `entry/`, so
>    renaming it would have been minting a new unread accessor inside the change
>    that exists to remove unread carriers.

### Stage H4 — stop typing generated catalog digests in tests

**Changes.** At `tests/deployment/test-project-deployment.cpp:1262-1310`, replace
the three literal sha256 expectations with a comparison against the digest the
test recomputes from the checked-in file under `schema/`, keeping the `identity`
assertions as they are.

**What replaces it.** Two independently derived values: the catalog digest comes
from the build-time generator (`scripts/embed_framework_schemas.py`), the
expectation from reading the file at test time.

**Guard.** The three catalog entries equal the sha256 of their files.

**Falsification.** Make the generator emit a constant digest instead of the
computed one: the red must land on the catalog-versus-file assertion for all
three documents. A red that lands on the `identity` assertion, or a
`REQUIRE_MESSAGE` about a missing catalog entry, does not count — that would
mean the case never reached the digest comparison.

**Estimate: 0.5 person-days.** Assumes the test can read `schema/` at run time,
which `tests/test-runtime-surface.py` already does from the same tree.

### Stage H5 — delete the two hash carriers nothing reads

**Changes.** Two independent deletions, both in the published Operator protocol.

1. Remove `journal_envelope_schema_hash` from `SessionManifest`
   (`schema/umbraflow-operator-v1.schema.json:144, 152`), from
   `SessionManifestSpec` (`modules/operator/source/operator/manifest.hpp:134`)
   and from the canonical serializer
   (`modules/operator/source/operator/manifest.cpp:95-96`); drop the field from
   its writers at `modules/service/source/service/product-lifecycle.cpp:273` and
   `modules/conformance/source/conformance/suite-support.cpp:264`, and from the
   fixtures at `tests/support/umbraflow/project-fixture.hpp:692`,
   `tests/operator/test-ledger.cpp:875, 2858`,
   `tests/operator/test-product-contract.cpp:184`,
   `tests/operator/test-state-contract.cpp:163, 651, 668` and
   `tests/deployment/test-project-directory.cpp:1318`.
2. Remove `$defs.RuntimeModelBindingRef`
   (`schema/umbraflow-operator-v1.schema.json:85-101`) outright.

**What replaces them.** Nothing. A field that no accessor exposes and no
consumer refuses carries no information; a `$defs` entry that no `$ref` reaches
and no source names carries none either. `session_manifest_hash` still covers
the manifest's remaining bytes, so no aggregate identity is weakened.

**Guard.** The existing SessionManifest field-coverage table in
`tests/operator/test-state-contract.cpp:668` — every declared member must move
`session_manifest_hash` — with the removed entry gone, plus a closure assertion
that reads the member names out of `SessionManifest::canonicalBytes()` and
requires every one of them to appear in that table. For the second deletion, a
schema check that every `$defs` entry in `umbraflow-operator-v1.schema.json`
**has a consumer**: it is either the target of a `#/$defs/<name>` reference
somewhere under `schema/`, or its bare definition name appears in a first-party
source under `modules/`, `tests/` or `entry/`.

> **Corrected 2026-08-15, before implementing this stage.** The guard stated
> here until now was a `$defs` **reachability** check — "every `$defs` entry is
> reached by at least one `$ref`" — and that shape does not work. Measured at
> HEAD `7d5ba0f` (see the correction note in [§2.3](#23-hash-fields-in-every-document-under-schema)):
> of 50 `$defs`, **12** are `$ref`-ed by nothing, so a reachability rule is red
> against twelve definitions on the day it is written and isolates nothing. It
> would have to be silenced with an eleven-name allowlist, and a rule that
> exempts by name whatever it is pointed at is the check-that-cannot-fail this
> repository's own `docs/pitfalls/checks-that-cannot-fail.md` forbids: the
> allowlist, not the code, would decide the outcome.
>
> The eleven other unreferenced definitions are not dead. They are top-level
> protocol shapes — `SessionManifest`, `PlanProposal`, `DecisionBasis` and the
> rest — that contract tests and runtime code name by **bare definition name**
> (`definition(schema, "SessionManifest")` at
> `tests/operator/test-state-contract.cpp:646` is the pattern), which is
> precisely why no `$ref` points at them. Being unreferenced is therefore normal
> for a protocol root and says nothing about whether anything uses it. The
> property that actually distinguishes `RuntimeModelBindingRef` from all eleven
> is that it has **no consumer of either kind**: no `$ref` and no name in any
> source. That is the property the guard now states, and it needs no allowlist —
> every entry passes on evidence found in the tree.

**Falsification.** Re-add the field to `SessionManifestSpec` and the serializer
without an accessor: the red must land on the "no orphan member" assertion, not
on a JCS byte-count or ordering assertion — a red from ordering would mean the
case is only noticing that the bytes changed.

> **Corrected 2026-08-15, while implementing this stage.** The guard above said
> "an assertion that the canonical manifest definition no longer contains
> `"journal_envelope_schema_hash"` (the inverse of the present
> `test-state-contract.cpp:651`)". Line 651 reads the **schema** text
> (`definition(schema, "SessionManifest")`), so the inverse of it is a
> schema-text assertion — and the falsification mutation prescribed here adds
> the member to `SessionManifestSpec` and to the C++ serializer, which does not
> touch the schema file. The guard as written could not go red on its own
> falsification, and it would only ever have guarded one hard-coded name. The
> closure assertion now specified reads the member names out of the canonical
> bytes and requires the table to contain each: it goes red by name on any
> member that reaches the serializer without a coverage row, which is the
> "no orphan member" property the falsification is asking for.

For the consumer check, two
mutations: re-add a `$defs` entry that neither a `$ref` nor a source names, and
the red must name that entry and only that entry; and take an entry that passes
only through its source consumers — `ProgressMarker`, whose single consumer is
`tests/operator/test-agent-audit-contract.cpp` — remove that one mention, and
the red must name `ProgressMarker`, proving the source-consumer half of the
disjunction is read rather than being a clause that always holds. Confirm before
starting that the consumer check is red **today** against
`RuntimeModelBindingRef` **and names nothing else**; a check written after the
deletion that has never been seen red is a check that cannot fail, and a check
seen red against twelve names has not isolated the one being deleted.

**Estimate: 1–2 person-days.** Assumes the `$defs` consumer check is a new rule
in `tests/test-runtime-surface.py` rather than a bespoke test — that file
already owns the repository's structural schema gates and already reads both
`schema/` and first-party sources from the same tree — and that
`modules/operator/**` and `tests/operator/**` are not being edited by another
lane at the time — both were mid-edit when this was measured.

### Total and ordering

**9.5–16.5 person-days for the framework half of stages H1–H5.** H1, H4 and H5
are independent of everything and of each other. H2 is independent of H1, H3,
H4 and H5. H3 must come last among the schema-pin stages only because it is the
one that empties `SCHEMA_AUTHORITIES` and deletes the rule; doing it earlier
would leave a partially populated table and a live rule with fewer entries,
which is fine but wastes a review cycle. H5 touches `modules/operator/**` and
`tests/operator/**` and should be scheduled when no other lane holds those
paths.

All estimates assume one engineer already familiar with both repositories,
include schema, fixture, documentation and gate changes, and exclude the
consumer-side work in §8.

## 5. What this plan deliberately does not do

- It does not touch the specification bundle pin, in either repository.
- It does not remove any file-closure, blob, plan, intent, state, evidence,
  capability, policy or session digest. §3.2 lists what stays and why.
- It does not add a compatibility path. Every stage renames the field, migrates
  the recorded bytes in the same change, and fixes every caller, per
  `CLAUDE.md`'s "break it rather than bridge it".
- It does not plan the consumer's half of anything.

## 6. Rows to lift into the consumer plan

These are the only work rows this document produces. They match the three-column
shape of the consumer plan's `§3` U-series sub-package tables
(`| ID | 工作 | 完成条件 |`) so they can be pasted without restructuring; ID
allocation belongs to that plan's owner. **Do not add them there as part of
reading this document.**

| ID | 工作 | 完成条件 |
|---|---|---|
| `H1` | Delete the trace payload schema digest: remove `k_traceSchemaHash`, `TypedTracePayload::schemaHash` and the trace envelope's `payload.schema_hash`; the already-emitted `schema` string is the format | An emitted envelope carries `"schema":"umbraflow-trace/v2"` and a payload object whose only member is `fields`; changing the schema string reddens the format assertion, and re-adding any payload digest reddens the shape assertion — each mutation lands on its own named assertion |
| `H2` | Replace `annotation_workspace_schema_hash` and `workspace_sqlite_schema_hash` with `annotation_workspace_format` and `workspace_sqlite_revision` across `tools/annotate/{publication,store}.py`, the positional release-manifest reader and every fixture | An unsupported revision is refused with a message naming supported and supplied; deleting the comparison reddens the same case for the opposite reason; and the positive control passes — editing a comment inside the workspace DDL leaves the whole gate green |
| `H3` | Replace `manifest_schema_hash` / `runtime_model_schema_hash` with `runtime_artifact_format` / `runtime_model_format`, replace Luau `model.schema_hash` with `model.format`, regenerate both example manifests, and delete `SCHEMA_AUTHORITIES` and `schema_authority_errors` once the table is empty | Format mismatch refused naming both numbers; a Luau/artifact format disagreement reddens at `finalizeRuntimeModel` rather than earlier in the parse; one mutated asset byte still reddens the closure refusal; and a cosmetic edit to `schema/umbraflow-runtime-v2.schema.json` leaves the gate green |
| `H4` | Stop typing the three generated framework-catalog digests in `tests/deployment/test-project-deployment.cpp` | The three catalog entries are compared against digests recomputed from the checked-in files; stubbing the generator's digest computation reddens that comparison for all three, not the identity assertion |
| `H5` | Delete `SessionManifest.journal_envelope_schema_hash` (written and serialized, but `SessionManifest` publishes no accessor for it and nothing refuses a mismatch) and the orphan `$defs.RuntimeModelBindingRef`, which no `$ref` reaches and no source names | Re-adding an unread manifest member reddens a "no orphan member" assertion rather than a byte-ordering one; a `$defs` **consumer** rule — every definition is either `$ref`-ed under `schema/` or named in a source under `modules/`, `tests/` or `entry/` — reddens on any definition with neither, is demonstrated red against `RuntimeModelBindingRef` **and nothing else before** the deletion, and its source-consumer half is shown live by deleting `ProgressMarker`'s single mention. A bare reachability rule is **not** acceptable here: 12 of the 50 `$defs` are `$ref`-ed by nothing because protocol roots are named rather than referenced |

Rows deliberately **not** opened, because the consumer plan already owns them:
Operator DDL schema identity (`U2a`) and `decision_basis_hash` having no reader
(`U12d` item 3).

## 7. Where the proposal is inaccurate

The proposal is right about the diagnosis and about which job is the problem.
Four of its specifics are wrong against the current code.

1. **`k_workspaceSqliteSchemaHash` is not kept synchronized by
   `tests/test-runtime-surface.py`, and cannot be.** The proposal lists it among
   six "hand-written framework schema digests" and says the surface test "exists
   partly to keep these copies synchronized". `SCHEMA_AUTHORITIES`
   (`tests/test-runtime-surface.py:227-253`) holds five entries and this is not
   one of them. `modules/operator/source/operator/runtime-installation.hpp:24-30`
   states the reason at the declaration: the value covers no checked-in file in
   this repository, so nothing here can recompute it. Consistently, the
   proposal's replacement list of format versions has no entry for it. The
   proposal is wrong; the code is right. Stage H2 handles it as a database
   revision, not a schema format.
2. **The proposal's inventory is incomplete in both directions.** It misses ten
   of the fifteen hand-authored production literals:
   `k_operatorDatabaseSchemaIdentity` (`ledger.cpp:478`) and the eight frozen
   `SchemaMigration::sourceIdentity` values (`ledger.cpp:834-876`), plus the
   plugin-environment pin (`tests/script/test-pure-data-boundary.cpp:440`). None
   of them is a schema digest, all ten stay, and two of them are already owned by
   consumer-plan row `U2a`. It also misses the two carriers nothing reads —
   `SessionManifest.journal_envelope_schema_hash` and the orphan
   `$defs.RuntimeModelBindingRef` — which are the cheapest removals in the whole
   inventory and the only ones that need no replacement at all.
3. **`k_traceSchemaHash` is a stronger removal candidate than the proposal
   states.** The proposal treats it as one of several schema pins to swap for a
   format version. It is in fact a per-payload schema slot that every first-party
   emitter fills with the same envelope-schema constant, that no first-party code
   reads, and whose format-version replacement already exists and is already
   emitted. It fails all four admission conditions, not two.
4. **Its baseline counts are not reproducible and two of them have not moved.**
   The proposal reports 108 hash-field occurrences in
   `umbraflow-operator-v1.schema.json` and 57 in
   `umbraflow-annotation-workspace-v2.schema.json` at `dc109bd`; a stated
   quoted-token count gives 110 and 62 at `dc109bd` **and the identical 110 and
   62 at HEAD**. The published schema hash surface has not changed at all since
   the proposal was written. Its "52 distinct hash-shaped schema field names"
   still holds under the narrower `hash|digest` pattern (53 including `sha256`),
   and its "seven JSON Schemas containing hash fields" still holds even though
   the repository now has thirteen schemas. Use the commands in §2, not the
   proposal's figures.

## 8. Named consumer-side consequences

Stated and stopped at. This document does not plan the consumer's half.

1. **Stage H3 breaks two consumer artifact generators and two committed
   manifests.** `runtime/tools/build-artifact.py:47-48` and
   `conformance/tools/build-fixture-artifact.py:40-41` hand-copy both digests
   from `modules/task/source/task/runtime-model-file.hpp`, and
   `runtime/artifact/runtime-artifact.manifest.json` and
   `conformance/runtime/artifact/runtime-artifact.manifest.json` carry the
   emitted values.

   > **Measured 2026-08-15, after implementing H3.** Five sites, and nothing
   > else in uf-chaos reads the RuntimeArtifact manifest format — no Luau, no
   > TOML, and nothing under `conformance/interface-lock/**`. Each must become:
   >
   > | Site | Today | Must become |
   > |---|---|---|
   > | `runtime/tools/build-artifact.py:47` | `MANIFEST_SCHEMA_HASH = "af9d5dd9…"` | `RUNTIME_ARTIFACT_FORMAT = 1` |
   > | `runtime/tools/build-artifact.py:48` | `RUNTIME_MODEL_SCHEMA_HASH = "72433231…"` | `RUNTIME_MODEL_FORMAT = 2` |
   > | `runtime/tools/build-artifact.py:253-255` | emits `"manifest_schema_hash"` before `page_model` and `"runtime_model_schema_hash"` after it | emits `"runtime_artifact_format"` and `"runtime_model_format"`, **both after** `page_model` — the JCS order changes, and a generator that only renames the members writes non-canonical bytes the Host refuses at the reader |
   > | `conformance/tools/build-fixture-artifact.py:40-41, 185-187` | the identical two constants and the identical emission | the identical two changes |
   > | `runtime/artifact/runtime-artifact.manifest.json` and `conformance/runtime/artifact/runtime-artifact.manifest.json` | committed bytes carrying both digests | regenerated by the two scripts above; both files' root hashes move, so anything pinning them moves with them |
   > | `runtime/README.md:112` | prose pinning `72433231…` as "框架当前 RuntimeModel schema" | prose naming RuntimeModel format `2` |

2. **Those copies were already stale before H3, and nothing caught it.** Both
   consumer scripts hold `RUNTIME_MODEL_SCHEMA_HASH = "72433231df31cdc18e8e88d21017a24e58c53b96e5b66c9d6b6bb96cf1647480"`,
   while `runtime-model-file.hpp` held
   `d47030ed22c65a224654f2fe7c7a594053f1bbc01b0b0663392ccde389d736fb` — verified
   at HEAD `9726ef6`. Every artifact those two generators produced was already
   refused, in a third copy that no gate on either side covered. `MANIFEST_SCHEMA_HASH`
   was still correct, which is what made the breakage silent: one of two
   transcriptions drifted. This is the failure the proposal predicts, already
   realised, and it was the strongest single argument for H3. **After H3 the
   consumer is broken in a different way — the field names no longer exist — but
   the repair is a one-time edit that cannot go stale again**, which is the whole
   point of the stage.
3. **Stage H1 has no consumer consequence.** uf-chaos contains no occurrence of
   `umbraflow-trace`; it does not consume the trace stream.
4. **Stages H2 and H5 have no consumer consequence.**
   `annotation_workspace_schema_hash` and `workspace_sqlite_schema_hash` appear
   nowhere in uf-chaos; both producer and acceptor are framework-side.
   `journal_envelope_schema_hash` and `RuntimeModelBindingRef` appear in uf-chaos
   only inside `docs/architecture/umbraflow-game-automation-final-design.md`, in
   prose, with no code on either side of it — so H5 costs the consumer one
   documentation correction and nothing executable.
5. **Consumer-authored schema digests are untouched.**
   `project_state_schema_hash`, `project_observation_schema_hash`,
   `project_tool_precondition_schema_hash`, `payload_schema_hash`,
   `tool_catalog_hash` and `effect_payload_sha256s`
   (uf-chaos `modules/project_state/registration.py:109-133`) are out of scope by
   §1.2 and no stage moves them.
6. **The consumer plan carries one stale statement about the out-of-scope bundle
   gate.** Its line 48 says `check-spec-bundle` does not run in `ci-local`;
   `tests/CMakeLists.txt:1155-1168` labels it `CI` and
   `scripts/ci-local.ps1:43` runs `ctest -L CI`. Correcting that sentence belongs
   to whoever takes the deferred bundle decision, not to this plan.
