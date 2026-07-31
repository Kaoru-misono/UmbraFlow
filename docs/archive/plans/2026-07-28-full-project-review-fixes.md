# Full-project review implementation follow-up

> **Vocabulary redirect (2026-07-31).** This document is a dated record and is not
> rewritten. Read `recognizer` / `RecognizerId` / `uf.recognizers` / `recognizerId`
> below as **element** / `ElementId` / `uf.elements` / `elementId`,
> `RecognizerDefinition` and `RecognizerVariant` as `CompiledElement` and
> `CompiledAppearance`, and `Variant` / `variant` as `Appearance` / `appearance`.
> `RecognitionCatalog` and `RecognitionRuntime` keep their names -- they name the
> activity. The schema ids moved with the rename: `umbraflow-authoring/v4`,
> `umbraflow-annotations/v3`, `umbraflow-trace/v2`. Canonical vocabulary:
> `CONTEXT.md` "Annotation model".

> Status: implementation complete and validated (2026-07-28).
>
> Source: [`2026-07-27-full-project-review.md`](../reviews/2026-07-27-full-project-review.md).
> This plan records the re-verification and implementation scope requested after
> the report-only review.
>
> Naming amendment (2026-07-28): the capture identity type is
> `CaptureSessionId`; existing `sessionId` members and trace wire fields remain
> unchanged.
>
> Application-metadata amendment (2026-07-28): the repository-root
> `manifest.txt` now owns the application name and version. CMake derives the
> project metadata from it and generates an entry-only `application-info.hpp`.

## Decisions

1. Retain `engine::EngineSession`. It is the stateful capability scope that owns
   a loaded recognition runtime and the capture/action/trace ports.
   `CaptureSessionId` is capture-session frame identity, not an identity for that
   object. The distinction is authoritative in `CONTEXT.md`.
2. Repair §3.1 structurally: `EngineSession` receives an `Observation` for
   resolve/find/act; `Observation` stores no session pointer. A private shared
   immutable token preserves the existing foreign-session invariant across
   moves without becoming public `EngineRunId` vocabulary.
3. Preserve the frozen `entry/m0-demo` acceptance reference. A finding whose
   only complete repair requires redesigning that target remains explicitly
   deferred rather than creating a second transitional API.
4. Treat the review as evidence, not a mechanical checklist. Incorrect claims
   are corrected inline; only problems that still exist are changed.

## Implementation slices

### 1. Lifetime and interface correctness

- Rename the task module's pure `TaskTraceSink` interface to
  `ITaskTraceSink`.
- Move page resolution and action lookup from `Observation` to
  `EngineSession`; remove the raw back-pointer and its lifetime contract, and
  compare the private stable token in all three operations.
- Update the production and test call sites and retain focused coverage for
  moved-from observations and foreign-frame evidence.

### 2. Medium architectural cleanup

- Render the source location promised by `formatRunError`.
- Centralize the workbench placement-withdrawal edit in the action layer.
- Split annotation identifier/geometry vocabulary out of `catalog.hpp`.
- Move the product name out of the mechanism-only `core` leaf.
- Put the workbench's platform-free model and canvas math at a legible layer.
- Rename controller files whose names describe D3D/guards that they do not
  contain.

`InputSession` remains deferred while `m0-demo` is frozen: retaining the old
free functions as wrappers would create two public action surfaces and would
not deliver the pairing guarantee that motivates the class.

### 3. Verified low-risk cleanup

- Return `Detection::label()` as a lifetime-bound borrow.
- Reuse the existing workbench draft snapshot instead of copying it per row.
- Prune stale texture-cache entries with document source pruning.
- Reuse the core UTF-8 decoder in controller text input.
- Consolidate annotation id/fingerprint codecs already duplicated across
  authoring and runtime parsers.
- Add the missing direct `ContentHash` include.
- Make Luau state ownership explicit at the FFI boundary.
- Convert the remaining leading-`void` signatures.
- Remove `gcinfo` from the sandbox.

Cross-entry `clientOriginDesktop`, `currentIoError`, and the dead m0 forwarder
stay unchanged where a complete consolidation would modify the frozen
acceptance target. Their status is recorded in the review rather than hidden by
a partial helper.

### 4. Documentation and validation

- Update the review status/evidence and both knowledge-language mirrors for
  changed APIs and file layout.
- Refresh code-atlas generated data after source changes.
- Run all four repository gates, configure when files move, build focused
  targets, build the full tree, and run the `CI` CTest label.

## Completion criteria

- [x] No stored raw pointer remains in `Observation`.
- [x] No pure interface in the changed engine/task surface violates `ITypeName`.
- [x] Every implemented review item has behavioral or compile coverage.
- [x] Frozen or invalid findings have an explicit disposition in the source review.
- [x] All required local validation commands exit successfully.

## Validation evidence

- Text normalization, C++ formatting, module-graph, and Safe C++ boundary gates:
  exit 0.
- Fresh `x64-debug` configure and full-tree build, including `umbra-flow`,
  `umbra-workbench`, and `m0-demo`: exit 0.
- `ctest --test-dir build/x64-debug -L CI --output-on-failure`: 15/15 passed.
- Code-atlas inventory verification and both generated knowledge renderers:
  exit 0.
