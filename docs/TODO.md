# Umbraflow execution status

This file is the active implementation ledger for the runtime annotation
rewrite. The current contract is frozen in
[`2026-08-09-runtime-model-contract.md`](plans/2026-08-09-runtime-model-contract.md).
The architecture ownership summary is in
[`ARCHITECTURE.md`](ARCHITECTURE.md). Older plans and worklists remain
reference material only until they are migrated or archived; they do not
override the current contract.

## Completed in this documentation pass

- [x] Declared `page-model.toml` to be a deployment envelope only. It does not
      carry screenshots, evidence, CandidateModel records, review history, or
      observed transitions.
- [x] Assigned complete `RuntimeModel` parsing and compilation to trusted
      Luau; limited C++ to pre-VM envelope facts, hashes, geometry, and IDs.
- [x] Defined Context, Surface, RuntimeState, Target, Locator, Reader, Binding,
      Action, Transition, Resolution, and Receipt ownership.
- [x] Defined `Present` / `Absent` / `Unknown` and `Resolved` / `Ambiguous` /
      `UnknownResolution` fail-closed behavior.
- [x] Documented the six Agent stages, revisioned decision queue, human review
      boundary, promotion, compile, replay, and rollback.
- [x] Removed current active documentation's descriptions of the retired
      object vocabulary as an implementation contract.

## Implementation backlog

### P0 — contract and schema authority

- [ ] Keep the four schemas and the semantic validator aligned with
      `docs/plans/2026-08-09-runtime-model-contract.md`.
- [ ] Add a contract check that rejects unknown fields and duplicate IDs, and
      verifies all cross-object references and layer-stack rules.
- [ ] Generate or validate Luau/Python/TypeScript bindings from the canonical
      contracts rather than maintaining independent enums.

### P1 — trusted runtime model and compiler

- [ ] Compile the full RuntimeModel from `page-model.toml` inside trusted Luau.
- [ ] Validate locator assets, hashes, readers, bindings, transitions, and
      action preconditions before making a model available to a task.
- [ ] Keep runtime packages free of offline frame/evidence stores.

### P2 — C++ envelope boundary

- [ ] Make the C++ reader consume only schema version, exact-file hash,
      geometry, target IDs, and surface IDs before VM startup.
- [ ] Add tests proving C++ does not interpret bindings, identity, locator
      thresholds, surface selection, or transition policy.

### P3 — resolver and action safety

- [ ] Implement same-cycle evidence, scene/overlay/interrupt stack resolution,
      and explicit `Resolved`, `Ambiguous`, and `UnknownResolution` results.
- [ ] Require framework-minted receipts tied to ticket, cycle, frame, model
      hash, binding, action, and proof.
- [ ] Reject all actions from unknown/ambiguous resolution, stale receipts,
      missing preconditions, and forged tables.

### P4 — transitions, check, and replay

- [ ] Keep declared transitions, observed transitions, and expected test
      transitions as distinct records.
- [ ] Make offline check/replay report coverage, unknown, ambiguity, invalid
      actions, unexpected transitions, and missing evidence.
- [ ] Ensure normal runtime loading never requires the offline frame corpus.

### P5/P6 — Agent backend and decision queue

- [ ] Store frames, observations, assertions, CandidateModels, patches,
      conflicts, provenance, and review audit outside the runtime package.
- [ ] Implement the six stages: Capture, Perception, Structure, Contract,
      Verification, Repair.
- [ ] Enforce revision checks, dry-run compile by default, and explicit patch
      decisions.
- [ ] Prevent bulk approval for high/critical identity or action changes.

### P7/P8 — fixtures and integration gate

- [ ] Keep the v2 fixture matrix green for ordinary scenes, overlays,
      interrupts, shared targets, collections, unknown OCR, stale receipts,
      unknown modals, and declared/observed transition divergence.
- [ ] Run schema, compiler, resolver, receipt, backend, UI, replay, and
      runtime-package boundary checks together before project migration.

### P9–P11 — project integration

- [ ] Rebuild project Context/Surface/Target/Locator/Reader/Binding data from
      the offline corpus; do not mechanically copy retired rows.
- [ ] Rework project task policies to branch on `Resolved`, retry or wait on
      `Unknown`, and stop for `Ambiguous`.
- [ ] Promote only compiled, validated model artifacts and keep their hashes
      with the project evidence ledger.
- [ ] Add regression cases for training confirmation, reward/loot details,
      reroll confirmation, shared anchors, settlement tails, enemy inspection,
      and the unknown dark modal.

### P12 — documentation cleanup

- [x] Rewrite the active architecture and execution ledger.
- [x] Rewrite the 2026-08-09 runtime, Agent, work-breakdown, and fixture plans.
- [x] Rewrite active pitfalls and the annotator backend guide around the
      current runtime/offline boundary.
- [ ] Migrate or archive older active plans and non-write-set indexes that
      still present the retired vocabulary as current. See the final P12
      handoff report for the exact files.

## Non-negotiable review gates

- [ ] No runtime artifact contains annotation screenshots or review metadata.
- [ ] No `Unknown` measurement is used as negative evidence.
- [ ] No `Ambiguous` or `UnknownResolution` can mint a receipt.
- [ ] No action is granted by a Target alone; it must come from the active
      Surface Binding and pass same-cycle preconditions.
- [ ] No high/critical action or identity patch is bulk-approved.
- [ ] Every promoted model has a compiler result, evidence replay result,
      reviewer audit, content hash, and rollback predecessor.

## Active-document migration boundary

The authorized P12 write set includes this file, `ARCHITECTURE.md`, all
`docs/plans/2026-08-09-*.md`, all `docs/pitfalls/*.md`, and
`tools/annotate/*.md`. Older plans under `docs/plans/`, plus
`docs/INDEX.md` and `docs/WORKLIST.md`, are outside that write set. They must
be migrated or moved under `docs/archive/` in a later authorized cleanup; the
old wording in those files is not a current runtime contract.
