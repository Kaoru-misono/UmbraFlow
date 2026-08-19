# Runtime and game-operator breaking rewrite

Decision date: 2026-08-09
Compatibility: intentionally breaking; no v1 compatibility code
Scope: `umbraflow-cpp` only; consumer projects are read-only

This is the upstream execution profile of the breaking rewrite: what it broke,
in what order, and what the result may not do. It states no contract version, no
digest and no progress. The rulings it once carried inline are frozen in
[`docs/decisions/`](../decisions/README.md) and are linked below; what this
repository publishes outward is
[`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

The RuntimeModel portion of this rewrite has since moved to Runtime v3. The
current field and behavior authority is the
[runtime model contract](../design/2026-08-09-runtime-model-contract.md) beside
`schema/umbraflow-runtime-v3.schema.json`. The disposition table and numbered
sequence below retain "Runtime v2" where they describe the rewrite that created
the boundary; they do not authorize a v2 reader or spelling.

## Product authority

The normative product input is the consumer's specification bundle, identified by
a semantic contract version. That version is not stated here: it is the
consumer's, and a copy of it in this repository can only be kept true by hand —
see
[2026-08-19 — a document holds no fact that something else verifies](../decisions/2026-08-19-documents-hold-no-foreign-facts.md).

What is checked mechanically is the part code consumes, not the prose. The
consumer's interface lock carries its own manifest pinning every schema and
vector by byte size and SHA-256, verified by the consumer's own suite. The
bundle's design documents are read by people.

- This repository states no exact-byte pin on that bundle:
  [2026-08-16](../decisions/2026-08-16-no-exact-byte-consumer-bundle-pin.md).
- Parity between our producer schemas and the frozen lock is a registered gate
  that takes the lock by path:
  [2026-08-17](../decisions/2026-08-17-interface-lock-parity-gate.md).
- The execution schedule is deliberately outside the bundle: pinning a status
  column is pinning progress.

If the contract version disagrees, implementation stops; if an interface-lock
vector's bytes disagree, the consumer's own gate stops it. Umbraflow does not
modify that consumer repository.

## Executable specification resolutions

Four places where this repository picked one side of a contradiction inside the
frozen specification and froze the choice upstream. They are not a second product
authority; each closes an explicit requirement of the consumer's design, and
checked-in upstream schemas only make the existing contract executable. Changing
any product field, disposition or ownership requires a new contract version, not
an edit here.

The four are frozen rulings and live in `docs/decisions/`:

1. [post-dispatch approval wait is recoverable](../decisions/2026-08-09-post-dispatch-approval-wait-is-recoverable.md);
2. [project and session roots have exact bytes](../decisions/2026-08-09-project-and-session-roots-have-exact-bytes.md);
3. [policy is Operator-owned](../decisions/2026-08-09-policy-is-operator-owned.md);
4. [consumer examples never become core contracts](../decisions/2026-08-09-consumer-examples-never-become-core-contracts.md).

The term itself was ruled on separately:
[2026-08-11 — `conformance` names the suite](../decisions/2026-08-11-conformance-names-the-suite.md).

## Non-negotiable boundaries

- Runtime v3 is pure UI semantics. Surface resolution and named
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
