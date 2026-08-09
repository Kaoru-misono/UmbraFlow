# Umbraflow upstream execution checklist

> Amended 2026-08-09: this checklist is derived from the
> [v1.7 breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md)
> and replaces the former consumer-project queue. Work in this repository does
> not modify a game project.
>
> Amended 2026-08-10: boxes below are ticked against a build and a test run
> that actually happened -- the tree had never been compiled before this date.
> Both independent reviews returned FAIL; their closed and open findings are in
> [the review outcome](reviews/2026-08-10-runtime-hardening-review.md), which is
> what the two unticked review boxes now wait on.

## G0 — contract and inherited baseline

- [x] Pin the four-document consumer bundle at root
      `90496173f8a920e59659b7c4568c8d04abe7b4d3d209db493a59fc3c55226ed0`.
- [x] Record base commit, rejected stash and the 101-entry dirty baseline
      manifest.
- [x] Assign exactly one disposition to all 101 inherited dirty paths
      (67 REWRITE, 34 DELETE).
- [x] Resolve post-dispatch approval recovery, exact registration/session root
      bytes, Operator-owned policy and consumer-example scope in the upstream
      authority.
- [x] Map every P/D/U/S/C/A requirement to an owner, schema location and exact
      CTest ID.
- [ ] Obtain final state/persistence and plugin-boundary review PASS on the
      upstream execution profile. Both ran on 2026-08-10, returned FAIL, and
      every finding is now closed or accepted with a reason; see
      [the review outcome](reviews/2026-08-10-runtime-hardening-review.md).
      The box turns on the re-review verdicts.
- [ ] Make active documentation point to this authority without copying old
      Context/Page/Target semantics.

## G1 — remove unsafe entrances and restore primitives

- [x] Delete old check/run/replay production CLI and file-frame projection.
- [x] Delete v1/UFR/envelope schemas, old runtime globals and compatibility
      adapters.
- [x] Keep and revalidate IDs, capture, cycle ledger, target generation,
      controller input/DPI, VM limits, TaskHost lifecycle and generic Trace.
- [x] Restore reduced regression assertions and register every planned contract
      in CTest before claiming coverage.
- [x] Register `contract-repository-surface` to reject retired files,
      commands, globals, parser duplication, consumer symbols and unconsumed
      Receipt proof fields.

## G2 — RuntimeArtifact and Runtime v2

- [x] Add exact RuntimeArtifact JCS schema, confinement-first verifier and
      generation-owned private finalize binding.
- [x] Implement one trusted Luau RuntimeModel parser.
- [x] Make UiTarget semantic-only and Binding the sole placement/variant owner.
- [x] Implement StateResolution then same-cycle BindingResolution.
- [x] Mint Host-owned expiring one-shot Receipt; keep production input closed.
- [x] Pass `contract-runtime-u01` through `contract-runtime-u08`.

## G3 — offline authoring

- [x] Implement minimal `annotation-workspace.sqlite` candidate, decision and
      replay ledger.
- [x] Publish immutable asset-complete releases; authoring publication is not
      production activation.
- [x] Prove production cannot read authoring DB/workspace/blob/replay roots.

## G4 — Operator and two plugin fixtures

- [x] Add minimal `modules/operator` and
      `operator-runtime.sqlite`.
- [x] Implement exact ProjectRegistrationManifest and SessionManifest JCS roots.
- [x] Implement lease/fence, opaque snapshots, idempotent commands, EffectivePlan,
      single-use approval, Operation transitions and reconciliation transaction.
- [x] Run one contract suite over two structurally different fixture plugins;
      core must contain no game symbol or branch.
- [x] Pass all local `contract-product-*`,
      `contract-state-*`, `contract-control-*` and `contract-agent-*`.

## Final gate

- [x] `ctest -N` lists every migration-report verification marked `CTEST`.
- [x] Focused security, crash, path-confinement and capability attacks pass.
      Path confinement is handle-based since 2026-08-10: reads and staging
      writes resolve once, ancestors are held, and a reparse point is refused
      by attribute rather than by tag.
- [x] The repository build skill completes `scripts/ci-local.ps1` with
      `GATE: PASS`.
- [x] Independent review finds no old reader, direct action path, consumer
      schema or fake-green unregistered test. None of the first three were
      found; seven fake-green tests were, and each replacement was falsified by
      removing the protection it names.

The real `uf-chaos + second game` attestation remains external and
`NOT_RUN`. It is not part of this upstream-only worktree and cannot be
replaced by fixtures; it gates consumer production mutation later.
