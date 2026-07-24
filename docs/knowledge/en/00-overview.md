# UmbraFlow Whole-System Architecture Overview

This document describes the executable architecture in the repository as of 2026-07-24, and uses the
current product plan to explain why these boundaries exist. The module graph is authoritative in
`docs/ARCHITECTURE.md`; the S0 annotation contract is authoritative in
`docs/plans/2026-07-22-annotation-design.md`; the A/B delivery cadence is authoritative in
`docs/plans/2026-07-21-product-form-and-roadmap.md`. Where the plan and the code do not fully agree,
this document puts the code's current state in the main text and places capabilities that have not
yet landed under "Extension seams".

## Responsibilities and boundaries

UmbraFlow's goal is not a general-purpose desktop macro tool but "strict-background, traceable,
visual-evidence-authorized" personal game automation. This goal splits the system naturally into two
halves: the platform-independent part answers "what was seen, and is an action permitted," while the
Windows part only answers "how to capture this target, and how to deliver an already-authorized
action to it." The two are composed only in `entry/`, which keeps recognition strategy from depending
on HWND and keeps the controller from knowing about page or recognizer.

In `docs/ARCHITECTURE.md` the dependency arrows mean "the consumer on the left depends on the
provider on the right." The trunk is:

```text
core
  ↑
domain
  ↑
vision      image
  \          /
   annotation
       ↑
     engine

controller (Windows) -> core, domain
script               -> core, domain
entry/cli             -> engine + controller
entry/workbench       -> annotation + engine + controller + image
```

`vision` and `image` are same-layer siblings and do not depend on each other; the upward arrangement
in the diagram indicates that product semantics accumulate layer by layer:

- `modules/core/manifest.txt` has no link dependency and provides only mechanisms such as `Result`,
  checked arithmetic, strong types, monotonic time, UTF-8, and contracts; it deliberately contains no
  game, page, image, or Windows policy.
- `modules/domain/manifest.txt` depends only on core and owns the cross-module values `Frame`,
  coordinate spaces, `Detection`, `ObservationLease`, `FrameId`, `TargetGeneration`, and
  `AutomationErrorKind`; these values describe the observation/action causal chain but do not explain
  screen semantics.
- `modules/vision/manifest.txt` depends only on core/domain and owns the deterministic Gray8
  conversion and the SAD matcher; it knows nothing about PNG, page, threshold business rules, or
  input delivery.
- `modules/image/manifest.txt` depends only on core/domain and keeps stb behind a private FFI; it
  handles PNG, channel layout, and rectangular crop, and does not decide what recognizer a crop
  belongs to.
- `modules/annotation/manifest.txt` depends on core/domain/vision and privately depends on image; it
  lifts pixel algorithms into project fingerprint, recognizer, page signature, evidence,
  authorization, the dual-document model, and a deterministic compiler.
- `modules/engine/manifest.txt` depends on core/domain/annotation; it owns the runtime loader, the
  `Observation` session, the capture/input/trace ports, and action timing, but contains no Win32
  types and does not duplicate annotation rules.
- `modules/controller/manifest.txt` is the only reusable module carrying `platforms = windows`; it
  owns window discovery, target continuity, WGC, DPI, and `PostMessageW` input, depends only on
  core/domain, and must not bypass the upper layers to decide clicks on its own.
- `modules/script/manifest.txt` is a standalone Luau 0.730 base module; it does not depend on engine,
  and `uf::script::Engine::runNumber` is still an unsandboxed, non-cancelable minimal executor, not
  the current product runtime entry point.

"Platform-independent except for controller" more precisely means the reusable trunk is
platform-independent. `umbra-workbench` is itself a Win32 + D3D11 GUI, and the real adapter behind
`umbra-flow run` is also Windows-only; but both live in `entry/` and do not leak platform types back
into domain, vision, image, annotation, or engine. This shape lets Linux/macOS build the pure modules
and lets CI replay frames with fake ports.

The system deliberately avoids the following cross-layer shortcuts:

- annotation does not discover windows, does not send input, and does not read authoring UI state.
- engine does not select targets, does not create WGC, does not write files directly, does not
  execute Luau, and does not accept unauthorized coordinates.
- controller does not load manifests, does not recognize templates, and does not interpret
  `ResolvedPage`.
- Workbench only authors/captures/previews/publishes and does not own runtime input capability;
  runtime reads only the generated manifest and the cropped templates, not the full authoring
  screenshots.
- `m0-demo` is not linked by engine or the CLI; it is a frozen acceptance reference, not a shared
  implementation repository.

There are currently three easily confused executable paths:

- `umbra-workbench` is the A1 authoring path: create/reopen a project, marquee-select, Preview, and
  publish assets.
- `umbra-flow run` is the B1 product path; it currently runs a C++ smoke flow that "waits for one
  page, finds one action, clicks once," and is not yet a full Luau-driven daily task.
- `m0-demo` is a substrate demo from the Rust→C++ port period; it has WGC + background input verified
  on real hardware, but is frozen.

## Key types and data flow

### The dual-document model for authoring and runtime

S0 does not let runtime consume the GUI's working files directly. The `AuthoringDocument` in
`modules/annotation/source/annotation/authoring-document.hpp` is the fully reopenable authoring source
of truth; the `RuntimeManifest` in the same directory is the minimal, read-only, deployable runtime
closure.

| Artifact | Owner and purpose | Needed by runtime |
|---|---|---|
| `annotations.toml` | Owned by Workbench; stores source, stable IDs, geometry, page links, regression | No |
| `assets/sources/<hash>.png` | The full original image, used for reopen, re-cropping, and regression | No |
| `assets/templates/<hash>.png` | A lossless crop generated from `template_rect` | Yes |
| `generated/annotations.runtime.toml` | recognizer, page, fingerprint, and asset closure | Yes |

The two schema constants are `g_authoringDocumentSchema`'s `umbraflow-authoring/v1` and
`g_runtimeManifestSchema`'s `umbraflow-annotations/v1`. `serializeAuthoringDocument`,
`parseAuthoringDocument`, `serializeRuntimeManifest`, and `parseRuntimeManifest` all require canonical
TOML; the reader re-serializes and compares byte for byte, so field order, UUID order, UTF-8, LF, and
the final newline are all part of the format contract.

`compileAuthoringDocument` lives in
`modules/annotation/source/annotation/authoring-compiler.cpp`. It first closes over the source and
recognizer references, validates the source hash/fingerprint, decodes source by source, and then
calls `generateTemplateAsset` to perform the BGRA crop, canonical PNG encode, and SHA-256. The
resulting `CompiledAuthoringProject` owns both the runtime manifest text and the deduplicated
`TemplateAsset`.

`ContentHash` and `sha256` are defined in
`modules/annotation/source/annotation/content-hash.hpp`. Both the source path and the template path
are determined by the lowercase SHA-256 of the bytes; `RecognitionRuntime::create` additionally
re-hashes each encoded template and validates the template dimensions against the recognizer geometry.
Content-addressed storage therefore provides deduplication, preservation of publish history, and
closure proof that "the bytes the manifest points to have not been swapped" all at once.

Workbench's `saveAndGenerateAuthoringProject` lives in
`entry/workbench/project-persistence.cpp`, and its publication order is fixed as source assets,
template assets, `annotations.toml`, and finally the runtime manifest. The last step is the runtime
commit point: by the time the new manifest is visible, all of the immutable assets it references
already exist. At present there is only an atomic replace per file, with no cross-artifact
transaction; if the last step fails, the new authoring document may temporarily coexist with the old
runtime closure, and the code does not pretend the rollback succeeded.

### The real run path from WGC to JSONL

The real composition root is `entry/cli/run-windows.cpp`. The full data flow can be compressed to:

```text
WgcCaptureSession::capture
  -> Frame(Bgra8) + ObservationLease
  -> bgra8ToGray8
  -> bounded matchTemplateSad
  -> AnchorEvidence
  -> PageResolver::resolve
  -> ResolvedPage + ActionDetection(Detection) + ObservationLease
  -> authorizeCoordinateAction
  -> ActionSink::click
  -> controller lease fencing + PostMessageW
  -> TraceEvent -> JSONL
```

Reading it step by step, the entry points and key types are as follows:

1. `engine::loadRuntimeProject`, in
   `modules/engine/source/engine/runtime-loader.cpp`, reads
   `generated/annotations.runtime.toml`, loads only the unique, referenced template hashes per the
   manifest, and then constructs `annotation::RecognitionRuntime`. Extra stale assets are ignored.
2. `runProduct` first resolves the page/action names, and only then declares DPI awareness, discovers
   the target, and creates the `WgcCaptureSession` and `DeliveryTarget`. A bad project fails before it
   touches the desktop.
3. `EngineSession::observe` first calls `FrameSource::validateTargetInstance` and then captures. The
   Windows adapter `WgcFrameSource` is only a thin forward to `WgcCaptureSession`.
4. The WGC `Frame` carries an immutable `FrameBuffer` owner, `SessionId`, `TargetGeneration`, a
   monotonically increasing `FrameId`, the capture instant, BGRA geometry, and a `CoordinateTransform`.
   `ObservationLease::forFrame` binds this same identity together with an action validity window of at
   most 750 ms.
5. `Observation::resolvePage` calls `RecognitionRuntime::evaluatePage`. It first requires the live
   `ProjectFingerprint` to be exactly equal to the manifest and requires the frame extent to equal the
   project `base_resolution`; P0 has no implicit scaling.
6. `withGrayFrame` converts the BGRA frame only once; `bgra8ToGray8` uses the integer
   `77*R + 150*G + 29*B` followed by a right shift of 8 bits. The template also passes through the
   same function when the runtime is created, so Preview and runtime do not have two separate
   grayscale kernels.
7. The page anchors call bounded `matchTemplateSad` in the catalog's stable UUID order.
   `RecognitionPolicy` provides the total pixel-comparison budget, the deadline, and a
   `std::stop_token` together; the budget for a single page evaluation accumulates across anchors.
8. `AnchorEvaluation::fromSadOutcome` turns the matcher result into `AnchorEvidence`, recording the
   recognizer ID, hit, the integer `sadScore`, the integer `maximumSad`, the matched rect, and a
   display-only confidence.
9. `PageResolver::resolve` evaluates every page: a page is a candidate only when all required anchors
   hit and all forbidden anchors miss. Zero candidates yield `UnknownPage`, one yields `ResolvedPage`,
   and multiple yield `AmbiguousPages`; there is no priority or heuristic tie-break.
10. `Observation::findAction` evaluates a single `ActionTarget` separately on the same frame. A miss
    is a successful empty `optional<ActionFound>`; a hit produces a `Detection`, which
    `ActionDetection::create` then binds to the project and a `RecognizerId`. `ActionFound` also stores
    the `PixelPoint` derived from either a template-local offset or the integer rectangle center.
11. `EngineSession::act` requires the caller to hand over the same `Observation`, `ResolvedPage`, and
    `ActionFound`. Once authorization succeeds, the click pixel is converted to `Point<ClientSpace>`
    via `pixelPointToFramePoint` and the frame's own transform, and the delivery edge then re-checks
    the target instance.
12. `ControllerActionSink` hands the client point and the original lease to the controller's
    `uf::click`. The controller checks the lease, the client bounds, and window liveness, and then
    uses `PostMessageW` to send move/down/up; on failure the adapter attempts `releaseHeld` to clean
    up any residual state.
13. engine synchronously emits a `TraceEvent` at observe, page outcome, action outcome, authorize,
    delivery, invalidation, and certain failure sites. `serializeTraceEvent` emits a fixed
    `engine-trace/v1` with a stable field order; `FileTraceSink` writes one line each time and flushes
    immediately, forming JSONL.

### Integer basis-point thresholds

`SimilarityThreshold`, in
`modules/annotation/source/annotation/catalog.hpp`, has a persisted range of `[0, 10000]`; `9000`
means 90.00%. The decision boundary uses checked integer arithmetic entirely:

```text
pixels  = templateWidth * templateHeight
maxSad  = floor((10000 - minSimilarityBp) * 255 * pixels / 10000)
hit     = sadScore <= maxSad
```

Equality counts as a hit. The float confidence is used only for display; it neither decides the hit
nor participates in candidate ordering. This way, identical pixels, geometry, and manifest do not
change the action across different builds due to float rounding.

## Design invariants

### Fail-closed

The system expresses "don't know" and "reject" separately, but neither can produce input:

- malformed/corrupt/non-canonical documents, broken references, hash mismatch, out-of-bounds
  geometry, duplicate page signatures, and incompatible fingerprints all return `InvalidResource` or
  a more specific structured error at the load/compile stage.
- `Cancelled`, `TimedOut`, and `ComparisonBudgetExhausted` are matcher stops and do not collapse into
  `hit=false`; a stop at any anchor terminates the entire page attempt.
- `UnknownPage` and `AmbiguousPages` are complete evaluation results, not exceptions, but only the
  host-created type `ResolvedPage` can enter `EngineSession::act`.
- The action recognizer must be an `ActionTarget` from the active catalog, and its `allowedPageIds()`
  must contain the resolved page; matching names cannot substitute for stable identity.
- The project fingerprint is checked both before recognition and at authorization; the bound target
  instance is additionally re-checked before delivery. Any failure occurs before the sink call.
- trace emission is not best effort. A failure to emit most engine evidence aborts the operation;
  after a click has already landed, the observation is invalidated before emitting, which prevents a
  trace failure from triggering a retry double-click.

### Two-layer action safety

D0/D1 in `docs/plans/2026-07-21-lua-task-model-grill-decisions.md` require that the observation
identity and the delivery identity cannot be decoupled. The current code implements this as two layers
rather than trusting a single upper-layer boolean:

1. Layer 1 is `authorizeCoordinateAction`, in
   `modules/annotation/source/annotation/authorization.cpp`. It compares the active project,
   fingerprint, resolved page, allowed page, and `ActionDetection`, and requires the `SessionId`,
   `TargetGeneration`, and `FrameId` of the page evidence, the `Detection`, and the delivery to all be
   equal; finally it calls `ObservationLease::validate` to check the full identity and expiry.
2. Layer 2 is the controller delivery fencing. The signature of `ActionSink` forces an
   `ObservationLease` to be passed; `ControllerActionSink` cannot reduce it to bare coordinates.
   `controller_detail::checkPointerPreconditions` re-checks the session, `TargetGeneration`, lease
   age, and client bounds before the post, and only then permits the single-target `PostMessageW`.

Code-level precision must be preserved here: the lease still carries `FrameId` at the second layer,
but the current `DeliveryTarget` has no "current frame" field, so the controller does not perform a
second independent `FrameId` equality check. The full same-frame comparison is performed by Layer 1,
and the fact that the lease reaches Layer 2 unchanged is pinned by an engine test. If the controller
is later made to compare frame freshness independently, the delivery state must be extended; do not
assume in the documentation that it already exists.

After an action succeeds, the `Observation` is invalidated immediately. It is non-copyable, and after
a move the source is also marked invalid; any surviving alias that calls `resolvePage`, `findAction`,
or `act` again returns `StaleObservation`. This turns "observe once, query multiple times on the same
frame, take one coordinate action, observe again" from a convention into a runtime contract.

### Determinism and boundedness

Determinism is not for an offline algorithm contest but to answer "why did it click here while
unattended":

- grayscale and SAD are both integer; the matcher scans in row-major order, and on a tie it keeps
  only the earliest position.
- the basis-point threshold uses an inclusive integer boundary, and checked overflow rejects the
  resource.
- recognizer/page/member arrays are ordered by stable UUID; uniqueness is decided only after every
  page has been evaluated.
- the authoring/runtime TOML has unique canonical bytes; the PNG encoder configuration and the golden
  bytes are pinned.
- source/template are addressed by the SHA-256 of the encoded bytes; identical crops can be
  deduplicated.
- `RecognitionPolicy` bounds comparison, deadline, and cancellation; a stop preserves the completed
  count and the reason.
- the trace schema, wire names, and field order are explicitly pinned and do not drift with C++ enum
  renames.

The monotonic wall clock participates only in the deadline, the wait, and the lease staleness fuse.
Its variation can only stop or reject an action earlier; it does not participate in normal hit/miss or
page ordering, so timing nondeterminism converges in the safe direction.

### Ownership, lifetime, and strict-background

`Frame` shares immutable pixels through `std::shared_ptr<FrameBuffer const>`; `GrayImage` is only a
read-only span view, and `withGrayFrame` guarantees that the backing gray buffer covers the
synchronous matcher call. `EngineSession` exclusively owns the three ports via `std::unique_ptr`; the
`EngineSession*` inside `Observation` is a borrow with an explicit "the session must outlive it"
contract and is checked during cross-session actions. The valid construction paths for `ResolvedPage`,
`ObservationLease`, and `ActionDetection` are restricted, so a caller cannot casually assemble an
aggregate that looks authorized.

strict-background is delivered by the controller's auditable, narrow boundary:

- `modules/controller/source/controller/platform/windows-input.cpp` calls `PostMessageW` only on the
  single bound HWND and explicitly rejects null and `HWND_BROADCAST`.
- `modules/controller/source/controller/input.hpp` lists the forbidden foreground/global input APIs;
  `scripts/check_safety.py` additionally scans `modules/`, `entry/`, and `tests/` and rejects any call
  that activates a window, injects globally, or moves the real cursor.
- There is no `SendInput` fallback and no "activate the window if the background attempt fails"
  degradation path.

Any new platform adapter must re-prove the `ActionSink` lease pass-through, single-target delivery,
and not stealing focus; implementing the interface itself does not automatically satisfy
strict-background.

## Collaboration with the rest of the system

Cross-layer data is deliberately kept as a narrow value or a port:

- On the inbound side, Workbench receives an imported PNG or a full BGRA `Frame` supplied by the
  controller and converts it into an `AuthoringSource` and an owned `AuthoringSourceAsset`; it
  delivers the document plus source bytes to annotation and gets back the compiled templates, the
  manifest, and Preview evidence.
- annotation passes only `GrayImage`, `PixelRect`, and the bounded policy down to vision; image
  handles PNG/crop only inside annotation's private implementation, and PNG codec types never appear
  in the engine public API.
- The CLI delivers `LoadedRuntime`, `EngineSessionConfig`, and the three owned ports to engine. engine
  does not receive an HWND, a DPI API handle, or a filesystem writer.
- The capture port crosses only `Frame` to engine; the action port crosses only
  `Point<ClientSpace>` + `ObservationLease` to controller. page/recognizer evidence does not sink down
  to the controller.
- The trace port crosses only `TraceEvent` to entry; newlines, file opening, flushing, and I/O error
  mapping stay in `entry/cli/file-trace-sink.cpp`.
- The fake `FrameSource`, `ActionSink`, and `TraceSink` use exactly the same engine surface, so
  offline CI can assert "zero delivery" without faking Win32.

`CoordinateTransform` is the live Client↔Frame relationship produced by capture; annotation geometry
is the integer `PixelRect` of the project `base_resolution`. P0 requires the two to be
identity-compatible and does not sneak base→live scaling into the existing transform. This separation
lets future resolution adaptation add an explicit stage rather than silently changing the controller's
client-coordinate semantics.

Errors cross modules uniformly through `Result<T>`/`Status` and preserve `AutomationErrorKind`. The
controller's native failure, annotation's resource failure, vision's stop, and engine's stale
observation can therefore reach the same CLI exit/trace boundary rather than being decided by parsing
an error string.

For finer-grained module navigation, continue with the existing knowledge documents:
`docs/knowledge/module-core.md`, `docs/knowledge/module-domain.md`,
`docs/knowledge/module-annotation.md`, `docs/knowledge/module-engine.md`,
`docs/knowledge/module-script.md`, `docs/knowledge/entry-cli.md`, and
`docs/knowledge/entry-workbench.md`.

## Testing strategy

The tests are layered as "pure algorithm → product semantics → port orchestration → Windows boundary"
to keep real-hardware instability from contaminating the deterministic contract, while making clear
that synthetic tests cannot replace real-hardware strict-background evidence.

| Test file | Contract it pins |
|---|---|
| `tests/vision/test-sad.cpp` | exact SAD, budget boundary, the three stops, poll interval, row-major tie, BT.601 integer gray |
| `tests/image/test-pixels.cpp` | channel conversion, stride-aware crop, template and frame sharing the gray kernel |
| `tests/image/test-png.cpp` | PNG round trip, identical input yields identical bytes, pinned golden bytes, quota and malformed input |
| `tests/annotation/test-catalog.cpp` | basis-point inclusive boundary, geometry/page closure, duplicate signature rejection |
| `tests/annotation/test-recognition.cpp` | Resolved/Unknown/Ambiguous without priority, any stop aborts the full resolution |
| `tests/annotation/test-recognition-runtime.cpp` | identical BGRA/Gray evidence, global budget, fingerprint, asset closure, action hit/miss |
| `tests/annotation/test-authorization.cpp` | project/page/recognizer/identity/lease/fingerprint all enter the layer-1 gate |
| `tests/annotation/test-authoring-document.cpp` | authoring canonical byte round trip and schema drift rejection |
| `tests/annotation/test-runtime-manifest.cpp` | runtime-only manifest, canonical escapes, and non-canonical input rejection |
| `tests/annotation/test-authoring-compiler.cpp` | pure compile, source closure, dedup, geometry, and work quota |
| `tests/annotation/test-template-asset.cpp` | crop→encode→hash→content-addressed path |
| `tests/annotation/test-regression-runner.cpp` | expected PageOutcome, suite cancellation/deadline, per-case budget |
| `tests/engine/test-runtime-loader.cpp` | published project load, missing/tampered template, manifest size cap |
| `tests/engine/test-session.cpp` | observe→resolve→find→act, zero-delivery spectrum, lease pass-through, invalidated handle, target-edge revalidation |
| `tests/engine/test-trace.cpp` | `engine-trace/v1` schema-first golden JSON, minimal record and escaping |
| `tests/cli/test-file-trace-sink.cpp` | one emit yields one JSONL line, write/open failure is not silent |
| `tests/controller/test-capture-wgc.cpp` | monotonic `FrameId`, geometry invalidation, capture option boundary |
| `tests/controller/test-capture-stall.cpp` | the exact boundaries of stale frame and stall timeout |
| `tests/controller/test-input-revalidation.cpp` | session/generation/age/bounds delivery fence and the check order |
| `tests/controller/test-input-guard.cpp` | forbidden API vocabulary and per-delivery audit |
| `tests/workbench/test-project-persistence.cpp` | assets-first/manifest-last publication, immutable collision, full reopen |
| `tests/workbench/test-preview.cpp` | Preview reuses runtime page/action evaluation and preserves the stop reason |
| `tests/workbench/test-source-ingestion.cpp` | imported/WGC source canonical encode, hash, provenance, and stride |

`tests/CMakeLists.txt` registers the Windows controller/workbench suites only when the controller
target exists. `tests/workbench/test-real-regression.cpp` is registered as `REAL` only when the local
`tests/assets/real-regression` exists, and real game screenshots do not enter the CI bundle.

The current synthetic-frame tests already cover the B1 fail-closed main chain, but `docs/TODO.md`
still lists as outstanding the Workbench real-hardware manual verification, the `umbra-flow run`
real-hardware smoke, the A1→B1 end-to-end, occlusion/minimization/CaptureStalled, and the 10–20 minute
long run. A green CI cannot be used to claim that these real-hardware properties have been accepted.

`tests/m0-demo/` continues to pin the frozen demo's parameters, guard, input-agent, pipeline, JSONL,
and shutdown behavior. They protect the acceptance reference from regressing but do not prove that the
engine path adopted the S0 schema.

## Extension seams

The roadmap uses S0/A1/B1/A2/B2/A3/B3 thin slices rather than building a complete GUI first and a
complete runtime afterward. The current landing points are as follows:

| Slice | Current implementation | Not yet done |
|---|---|---|
| S0 | domain identity/geometry, vision bounded SAD, image deterministic PNG, annotation dual schema/page/action contract | design is locked; later schema changes require a new authority |
| A1 | `umbra-workbench`, source ingestion, draft/history, canvas, all properties, Preview, save/reopen | real-hardware GUI acceptance; a more mature multi-page/sample UX |
| B1 | `modules/engine` ports/loader/session/trace, `umbra-flow run`, synthetic fail-closed CI | real-hardware smoke and A1-asset end-to-end |
| A2/A3 | the annotation backend can already express multiple pages, regression, and Unknown/Ambiguous | multi-page editing experience, sample management, batch and regression UX |
| B2/B3 | `EngineSession` already leaves an observe/find/act/wait surface and a popup sweep hook | Luau binding, sandbox/cancel/quota, minimal popup sweep, full daily and long runs |

When extending, enter from the corresponding authority and the existing seam:

- Changes to the S0 schema, recognizer, page, asset, or authorization first revise the successor
  authority of `docs/plans/2026-07-22-annotation-design.md`, and then change `RecognitionCatalog`, the
  compiler, the runtime, Preview, and regression; you cannot merely add a field in the GUI.
- A2/A3 attach to `AuthoringDocument`, Workbench draft/history, `runPreview`, and
  `runAuthoringRegressions`, and should not establish a second editor-only manifest or matcher.
- B2 binds the Luau capability to the existing `EngineSession` surface.
  `docs/plans/2026-07-21-luau-integration-plan.md` states that only the first two steps of the base
  integration are currently done; the sandbox, non-swallowable cancellation, allocator quota, and host
  bindings are still open, and `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md` is the
  implementation checklist.
- D6's `EngineSession::sweepKnownPopups` is currently a no-op. P0-C fills a minimal known-popup sweep
  into each wait cycle; only at P1 does it add registration order, first-match, re-entrancy
  prohibition, and `max_hits` per `docs/plans/2026-07-21-lua-task-model-grill-decisions.md`.
- P1 resolution adaptation should add an independent, traceable Base→Live transform between the
  annotation base geometry and the live FrameSpace; do not tamper with the existing
  `CoordinateTransform`'s Client↔Frame responsibility, and do not scatter float scale into the
  recognizer.
- The P2 tray app, task lifecycle, overlay, and HTML trace report are built on the engine
  port/session/event seam. The current `TraceEvent` is a versioned JSONL vocabulary, not a complete
  replay package; adding a resource snapshot or subscription semantics requires an explicit
  schema/version design.
- The P3 second platform only needs to implement the semantic surface of `FrameSource` and
  `ActionSink`, but it must still provide platform-level proof of target-instance continuity, lease
  fencing, and strict-background.
- The entry point for hardening Workbench publication into a transaction is
  `saveAndGenerateAuthoringProject` and the platform file-publication seam. Before adding a generation
  directory, a journal, or an equivalent protocol, you must maintain assets-first,
  runtime-manifest-last.

The basis for freezing `m0-demo` is `docs/plans/2026-07-20-m0-demo-port-deviations.md` and
`docs/plans/2026-07-23-engine-architecture.md`. It retains the already-real-hardware-verified WGC,
background PostMessage, guard, elevated input-agent, and shutdown reference, but bypasses the S0
authoring/runtime schema, the page/action capability, and the basis-point threshold; its `--threshold`
semantics are also explicitly not migrated. Continuing to add product features to the demo would form
a second authorization and asset path.

Therefore, new product capabilities enter the `annotation -> engine -> entry` trunk; if real-hardware
results expose Windows semantics that the demo already solved but the trunk lacks, copy only the
verified semantics into the controller/adapter and add trunk tests, without letting engine link the
frozen demo. Only after the trunk reaches real-hardware capability parity is the demo retired
separately, as planned.
