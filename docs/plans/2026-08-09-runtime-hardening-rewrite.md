# Runtime v2 and game-operator breaking rewrite

Status: active upstream implementation authority
Decision date: 2026-08-09
Compatibility: intentionally breaking; no v1 compatibility code
Scope: `umbraflow-cpp` only; consumer projects are read-only

## Frozen product authority

The normative product input is the consumer's specification bundle at **contract
version v1.18**. That is a semantic version, not a digest: two repositories agree
when they implement the same contract version, and a digest can only report that
two byte strings differ.

- The consumer repository is `github.com/Kaoru-misono/uf-chaos`; the bundle
  documents live under its `docs/architecture/`.
- Five documents are in it: the main design, the project-layer design, the
  requirements matrix, the failure/recovery audit and the interface contract
  lock. `parallel-implementation-plan.md` is deliberately outside — it is an
  execution schedule, and pinning a status column is pinning progress.
- **What is checked mechanically is the part code consumes**, not the prose:
  `conformance/interface-lock/v1.18/` in that repository carries its own
  manifest pinning every schema and vector by byte size and SHA-256, and the
  consumer's `tests/contracts/test_interface_lock.py` verifies it — including a
  refusal of CRLF. Those bytes are read by a validator; the five documents are
  read by people.

> **Amended 2026-08-17: the vector half of the residual risk below now has a
> gate, and the gate is red.** `tests/contracts/test_interface_lock_parity.py`
> was written to compare this repository's producer schemas against the bytes
> the consumer froze, and it was registered in no CTest target: it had never run
> outside a hand invocation. It is now `check-interface-lock-parity`.
>
> It takes the lock by path and this repository declares no default for it, so
> the defect the ruling below removed does not come back. The path is a CMake
> cache entry, `UF_CONSUMER_INTERFACE_LOCK`, and not an environment variable,
> for the reason `CONTEXT.md` gives for `project --frames-root`: nothing here
> reads the environment, and a run whose outcome depends on ambient state has no
> record of what produced it. Undeclared, the gate is registered and reports
> itself skipped by name; declared, it carries the CI label, because a
> divergence report nobody is required to read is the defect an unregistered
> test already was.
>
> **Measured 2026-08-17 against `v1.18`: four of the five vectors agree and
> `single-step-tool.json` does not.** The frozen vector's `/valid/schema` is
> `umbraflow-declarative-single-step-tool/v1`; this repository's producer
> requires `umbraflow-declarative-workflow-tool/v1`. `/valid` is missing `states`
> and `steps` and carries a stale `ui_action`; all six of `/valid/bounds` are
> spelled differently — `max_dispatches`, `max_observations` and `timeout_ms`
> against `maximum_dispatches`, `maximum_observations`, `maximum_elapsed_ms`,
> `maximum_states`, `maximum_steps` and `maximum_waits`.
>
> Both repositories still call this contract v1.18 while disagreeing about the
> declarative tool document, which is the disagreement the top of this section
> says stops implementation. Nothing here rules on which side is right — that is
> owed by whoever next revises the lock, and it is the same obligation the
> residual-risk paragraph below already carries. What changed is that the
> disagreement is now printed by a gate instead of being a thing nobody had run.

> **Ruled 2026-08-16: the exact-byte bundle root pin is removed**, executing
> Stage 1 of the hash management proposal. Three measurements decided it, and
> each is checkable rather than a matter of taste.
>
> **It was a CI-labelled test that depended on another repository's absolute
> path.** `check-spec-bundle` carried `LABELS "CI"` in `tests/CMakeLists.txt`,
> so it ran inside `ctest -L CI` and therefore inside `GATE: PASS` — the sentence
> claiming it stayed out of `ci-local` was wrong, and the archived hash-cleanup
> plan had already caught that. It found the bundle by regex-matching an absolute
> path out of the prose above. On any machine without `E:/umbraflow-projects/uf-chaos`,
> this repository's own gate could not pass. A reusable project foundation cannot
> have that.
>
> **Half of it never fired.** The digest printed here was only ever used to
> locate the checkout; the value actually compared was the constant inside the
> script. Mutating this document's root to a different digest left the gate green
> and printing `VERIFIED`. That was recorded as a live divergence owned by
> `U12d`, and it disappears with the pin rather than needing a repair.
>
> **What it guarded is guarded better elsewhere.** The interface lock's normative
> content — schemas and vectors — is byte-pinned by the consumer's own
> interface-lock manifest, in the consumer's own automated suite. Pinning the
> Markdown on top of that was a second spelling of one thing. The other four
> documents are design prose, and nine known textual divergences sit inside them:
> every one was blocked from repair by the pin, and repairing prose was never
> what byte equality was for.
>
> The residual risk is stated rather than hidden: outside what the interface-lock
> vectors cover, there is no longer a mechanical check that both repositories
> read the same contract version. Closing that is a review obligation on the
> version number above, and the coverage question — which of the lock's prose
> conventions must be mechanically held and which are left to review — is owed
> by whoever next revises the lock.

> Amended 2026-08-12 (v1.18). Three things moved together.
>
> The **location** stated here was wrong. A v1.13 amendment claimed the
> `E:/umbraflow-projects/uf-chaos/` checkout was deleted and moved the pin to
> `E:/github/uf-chaos/`. The opposite is true: measured on 2026-08-12,
> `E:/github/uf-chaos/` does not exist and `E:/umbraflow-projects/uf-chaos/` is
> the live checkout, clean and pushed to `github.com/Kaoru-misono/uf-chaos`. The
> full check therefore could not run at all, and the CTest registration then
> read no consumer bytes, so nothing noticed for three bundle versions.
>
> The **membership** changed. `interface-contract-lock.md` joins as a pinned
> member: it is the wire contract two repositories implement against in parallel,
> so this framework's correctness does depend on its exact bytes.
> `parallel-implementation-plan.md` is deliberately *not* pinned. It is an
> execution schedule; its status columns move as work proceeds, and pinning it
> would red this gate on every increment of progress until nobody read it again.
> The consumer's G0 was rewritten from six documents to five in the same change.
>
> The **contents** changed: v1.14 through v1.16 froze the agent-first baseline and
> the wave-zero interface; v1.17 fixed seven contract defects found by review and
> measurement; v1.18 fixed five more, the largest being that the rejection-to-code
> map was neither total nor deterministic, so two conforming implementations could
> refuse the same document with different normative codes. See the interface
> lock's own change record.

If the contract version disagrees, implementation stops; if an interface-lock
vector's bytes disagree, the consumer's own gate stops it. Umbraflow does not
modify that consumer repository. This file is the upstream execution profile and records
four executable specification resolutions derived from explicit v1.9 clauses; it
does not add product behavior or import consumer-specific schemas into core.

## Executable specification resolutions

> Renamed 2026-08-11 from "executable conformance resolutions". `conformance`
> now names one thing in this repository, the exported suite under
> `conformance/`, and a word given both to a test suite and to a class of
> specification fork is the defect that rename exists to remove. The suite took
> the word because every future consumer reads its name while this term is read
> by this repository's maintainers, and because a resolution of a contradiction
> inside a frozen specification is what these four are. Nothing about their
> content changed. Deciding artifact: the conformance rename of 2026-08-11; the
> term is recorded in `CONTEXT.md`.

These are not a second product authority:

- the post-dispatch edge closes the main design §8.4 requirement that
  cancel/deadline/lease loss after dispatch enters reconciliation;
- the manifest schemas implement the main design §12 and Phase 0 requirement
  for canonical, language-independent registration/session roots;
- Operator-owned policy follows main design §5.3's pure plugin capability list
  and policy order;
- consumer-example exclusion follows main design §6's explicit project
  ownership boundary.

Changing any product field, disposition or ownership still requires a new
consumer bundle root. Checked-in upstream schemas only make the existing
contract executable.

### 1. Post-dispatch approval wait is recoverable

The unique Operation transition table gains this fail-closed edge:

```text
awaiting_approval (plan_frozen_at != null) -> reconciling
  guard: cancel, deadline, lease loss, restart epoch change, or approval can no longer be obtained
  effect: freeze further input; retain frozen plan and mutation-chain lock
```

It never transitions directly to cancelled/expired or releases the mutation
chain. Reconciliation alone may establish a business terminal disposition.

### 2. Project and session roots have exact bytes

`ProjectRegistrationManifest` has exactly these fields:

```text
project_registration_format
plugin_id
plugin_hash
tool_catalog_hash
project_state_schema_hash
project_observation_schema_hash
project_tool_precondition_schema_hash
reconcile_payload_schema_manifest_hash
journal_event_schema_manifest_hash
baseline_event_type
project_artifact_roots[] { name, root_hash }
```

`project_registration_format` is a generation, not the digest of the
registration schema file. The digest it replaced compared a value with itself —
the loader derived it once and handed the same local to the document and to the
schema owner judging the document — while moving every registration root on any
cosmetic edit to `schema/umbraflow-project-registration-v1.schema.json`.

`plugin_id` and every field above participate in the root. Artifact-root names
are non-empty, unique, and sorted by UTF-8 bytes. The manifest bytes are RFC
8785 JCS UTF-8 with no BOM or trailing newline.
`project_registration_hash = sha256(exact manifest bytes)`. There is no second
`project_artifact_roots_manifest_hash` shape.

`SessionManifest` has exactly these fields:

```text
host_protocol_schema_hash
runtime_model_schema_hash
runtime_model_artifact_root_hash
operator_protocol_schema_hash
project_registration_hash
policy_artifact_hash
agent_profile_hash
```

> Amended 2026-08-15: `journal_envelope_schema_hash` was removed from
> `SessionManifest` — it was written and serialized but had no accessor, no
> reader and no refusal. See Stage H5 of
> [the framework hash cleanup](2026-08-14-framework-hash-cleanup.md).

It uses the same JCS byte rule and
`session_manifest_hash = sha256(exact manifest bytes)`. Version labels are
diagnostic only.

### 3. Policy is Operator-owned

Projects may supply a content-addressed policy artifact as deployment input.
Only Operator parses and evaluates it. ProjectPlugin receives no policy
capability or hidden policy input; plugin determinism is therefore bounded by
its explicit arguments and pinned ProjectRegistration.

### 4. Consumer examples never become core contracts

The upstream core does not implement `chaos.*` tool names, ChaosSession,
Chaos Journal event names, Chaos content states, or any other game entity.
The main design's consumer examples are non-normative for upstream source.
Likewise, generic structs repeated in the consumer document do not create a
second wire schema: the checked-in upstream schemas are the sole executable
shape, and consumer payloads remain schema-validated opaque data.

Two structurally different upstream fixture plugins are the local framework
gate only. They do not satisfy the real dual-game gate. That later cross-repo
attestation must run the same conformance suite against two real, independently
owned registrations, record both exact `project_registration_hash` values,
and pass before either consumer opens production mutation. It is deliberately
not claimed or executed by this worktree.

## Non-negotiable boundaries

- Runtime v2 is pure UI semantics. Surface resolution and named
  UiTarget/Binding resolution are separate and fail closed on Unknown.
- UiTarget is semantic identity only. Binding owns every actionable placement
  and variant, including a fixed placement.
- RuntimeArtifact has one load path: C++ verifies confined manifest/files and
  generation; one trusted Luau parser interprets complete model semantics.
- Phase 1 may mint a Host-owned opaque Receipt but has no production action
  entry point. Only `Host.deliver(authority, receipt)` consumes it.
- Operator owns lease/fence, snapshots, commands, frozen plans, immutable
  authority decisions, policy, approvals, operations, and reconciliation.
- ProjectPlugin is a five-function data boundary. Core has no game-name branch.
- Production uses `operator-runtime.sqlite`. Offline authoring uses a separate
  `annotation-workspace.sqlite`; production cannot attach or read it.
- Production model/session/trace artifacts never carry annotation screenshots.
- No bus, distributed lease, dynamic ABI, DI container, generic workflow
  engine, compatibility reader, alias, fallback, or dual-write path is added.

## Reproducible inherited baseline

Implementation started from:

- branch: `design/annotation-system-v2`
- base commit: `1b89760227e070fcf88f2778c312dce5cc9d87b4`
- inherited dirty entry count: `101`
- inherited dirty entries root:
  `55444b02a8ace9fe7493e5175618ef0a67d87402087874b7972187ac71ed8ac7`
- machine-readable manifest:
  [`2026-08-09-runtime-migration-baseline.manifest.json`](2026-08-09-runtime-migration-baseline.manifest.json)
- exhaustive inherited-diff disposition:
  [`2026-08-09-runtime-migration-disposition.manifest.json`](2026-08-09-runtime-migration-disposition.manifest.json)
- disposition root:
  `fdf9d26ff6bbcc2cd56c3c5c8f35ea9b488d78f24cdd8b2a5b5de8d9fc5aa09d`
- rejected historical stash:
  `stash@{0}` / `7d9a14e976b940d24e609c9d9d396c19c0a6d00c`

The dirty manifest describes the exact inherited state before these G0
authority corrections. It is an audit baseline, not an accepted implementation
or a reason to preserve a file. Do not reset, overwrite, or attribute quality
by author; classify each subsystem against the frozen contract.

## Migration disposition

| Disposition | Inherited area |
|---|---|
| KEEP | strong IDs/generation, checked arithmetic, frame/space/hash primitives, capture and ObservationLease, cycle ledger, target revalidation, controller input/DPI, VM sandbox/cancellation/resource limits, TaskHost lifecycle, privileged Explore primitive, generic Trace primitives |
| REWRITE | RuntimeArtifact loader/finalize binding, Runtime v2 schemas and Luau parser/resolver, Receipt mint/deliver boundary, annotation SQLite/publication, ProjectRegistration/session manifests, Operator ledger, and their tests |
| DELETE | old check/run/replay production CLI, file-frame projection, Page/Element/Hit/UFR/Context-truth, v1/envelope schemas, direct action globals/FFI, caller model/measurement/effect seams, obsolete trace replay consumers, JSONL annotation state and compatibility adapters |
| ALREADY_SATISFIED | only a behavior that already passes the new named attack/contract test; deletion or a suggestive v2 filename alone does not qualify |

The detailed requirement-to-owner/schema/test map lives in
[`2026-08-09-runtime-migration-report.md`](2026-08-09-runtime-migration-report.md).

## Ordered delivery

1. Freeze this authority, reconcile documentation drift, classify the inherited
   diff, and make every planned contract visible to CTest.
2. Remove unsafe/obsolete entry points and restore trusted primitive regression
   coverage without restoring old semantics.
3. Implement RuntimeArtifact, the unique Runtime v2 parser, two-stage
   resolution, Host binding, and opaque one-shot Receipt with no production
   input path.
4. Implement SQLite authoring publication with immutable releases, replay gate,
   and deployment-boundary verification.
5. Implement minimal Operator Core and the exact ProjectRegistration/session
   manifests; run one suite over two structurally different fixture plugins.
6. Run focused gates, full local CI, and an independent final review.

No consumer project migration and no production mutation is part of this
worktree.

## Verification

All configure/build/test commands use the repository build skill. The final
gate is:

```powershell
cmd /c "call .claude\skills\build-project\script\windows\build-env.bat && pwsh -NoProfile -File scripts\ci-local.ps1"
```

`ctest -N` must list every verification ID marked `CTEST` by the migration
report. IDs marked `EXTERNAL` are cross-repository attestations and cannot be
made green by a fixture. Missing
tests, a second model parser, an unconsumed proof field, a consumer-specific
core symbol, or an obsolete action entry point fails the gate even if the
project compiles.
