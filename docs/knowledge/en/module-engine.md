# modules/engine Architecture Knowledge

This document explains the runtime orchestration already implemented by `modules/engine`. Design
background is in `docs/plans/2026-07-23-engine-architecture.md`; related decisions about frame
semantics, leases, and error tracing are in D0, D1, and D4 of
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md`. Capabilities that have not yet landed are
collected at the end.

## Module Responsibilities

`modules/engine` is the orchestration layer of the product runtime. It turns a published annotation
project into an executable recognition runtime, obtains frames from a frame source already bound to
a target instance, resolves pages and action targets on the same frame, invokes annotation's
authorization policy, and finally hands the authorized client-space click to the input port.

It owns four kinds of product semantics:

- Publication reading: `loadRuntimeProject` reads the runtime manifest and the template assets it
  references, and constructs `annotation::RecognitionRuntime`.
- Single-frame decisions: `EngineSession::observe` produces an `Observation`; both page resolution
  and action lookup read the same `Frame` held by that handle, and never implicitly capture again.
- Coordinate action delivery: `EngineSession::act` threads page evidence, action evidence, frame
  identity, lease, live fingerprint, and target-instance recheck into a single fail-closed path.
- Runtime evidence: `TraceEvent` and `serializeTraceEvent` define a stable, versioned JSON record;
  `ITraceSink` leaves the persistence policy to the composition root.

The engine deliberately does not own the following capabilities:

- It does not discover windows, does not create WGC, and does not call Win32 input APIs.
  `modules/engine/manifest.txt` depends only on `core`, `domain`, and `annotation`, not on
  `controller`, so the engine can be built and tested on non-Windows hosts and in offline CI.
- It does not decide how a specific platform performs strict-background delivery. It records the
  contract through `IActionSink`; the Windows implementation lives in
  `entry/cli/platform/controller-action-sink.hpp`, and the platform capability is still owned by
  controller.
- It does not own target selection, DPI awareness, window geometry, or Ctrl-C registration. These
  composition responsibilities live in `entry/cli/run-windows.cpp`.
- It does not edit or publish annotation projects, nor read the authoring document. It only reads
  `generated/annotations.runtime.toml` and the runtime templates within that manifest's closure.
- It does not implement a Luau host, task queue, pause/resume, event subscription, or resident
  process lifecycle. The existing `uf::script::Engine` in `modules/script/source/script/engine.hpp`
  is still an independent, minimal Luau executor that currently neither depends on nor binds
  `uf::engine::EngineSession`.
- It does not copy recognition algorithms and authorization rules into itself. Page/action
  recognition is performed by the annotation runtime, and authorization of page-allow relationships,
  fingerprint, and frame identity is validated by annotation.
- It does not define any storage policy beyond the trace file format. The engine's serializer does
  no I/O and appends no newline; opening the JSONL, writing each line, and flushing live in
  `entry/cli/file-trace-sink.cpp`.

The reason for this boundary is not abstraction for its own sake: strict-background input can only be
honored by the platform layer, while recognition and authorization must be reproducible in CI that
has no desktop and no HWND. The narrow ports let the safety-critical runtime ordering be tested with
fakes, while keeping the platform code thin and auditable.

## Runtime Flow

### Ports

The public ports are concentrated in `modules/engine/source/engine/ports.hpp`.

- `IFrameSource::capture() -> Result<Frame>` obtains a frame from an already-bound target.
- `IFrameSource::validateTargetInstance() -> Status` rechecks that what is bound is still the same
  target instance. The engine calls it once before capture and again before delivery; the latter
  closes the window where "the HWND is reused or the target is replaced after observation".
- `IActionSink::click(Point<ClientSpace>, ObservationLease const&) -> Status` receives the client
  coordinate and the original lease. The lease pass-through is part of the interface; an adapter
  cannot pass only the coordinate, otherwise the controller layer cannot perform the second layer of
  session/generation/age fencing.
- `ITraceSink::emit(TraceEvent const&) -> Status` is a synchronous, fallible evidence port. A failure
  is not a best-effort warning but aborts the current engine operation.

All three ports are non-copyable and non-movable, with a virtual destructor supporting
implementations provided by the composition root. `EngineSession` owns them exclusively through
`std::unique_ptr`, so the port implementations and the platform resources within them share the
session's lifetime.

The Windows product entry uses two thin adapters:

- `WgcFrameSource` in `entry/cli/platform/wgc-frame-source.hpp` owns a `WgcCaptureSession`, and its
  two methods directly forward `capture` and `validateTargetInstance` respectively.
- `ControllerActionSink` in `entry/cli/platform/controller-action-sink.cpp` owns `DeliveryTarget`,
  `HeldInputs`, and `AuditLog`, and passes the lease unchanged to controller's `uf::click`. On
  failure it calls `releaseHeld` to compensate for any pressed state that may remain, and preserves
  the original error.

### Loading a Runtime Project

The entry point is `loadRuntimeProject(projectRoot) -> Result<LoadedRuntime>` in
`modules/engine/source/engine/runtime-loader.hpp`. The actual path is as follows:

1. Assemble the path `generated/annotations.runtime.toml`.
2. `readCappedFile` first uses `file_size` to quickly reject files that obviously exceed the limit,
   then actually reads in 64 KiB chunks; the actual byte count is still bounded, so a file that
   grows after the stat cannot bypass the cap.
3. The manifest cap is fixed at 16 MiB by `k_maximumRuntimeManifestBytes`.
4. The text is handed to `annotation::parseRuntimeManifest`; the engine does not write a separate
   TOML parser.
5. Referenced files are read in the order of the manifest's `assets()`. The same `ContentHash` is
   loaded only once; on-disk files not referenced by the manifest are ignored, allowing the
   content-addressed store to keep history.
6. The template read cap is 64 MiB. This constant in
   `modules/engine/source/engine/runtime-loader.cpp` deliberately mirrors the image module's PNG
   cap, because image is a private dependency of annotation, and the engine cannot cross the module
   boundary to include its header.
7. `annotation::RecognitionRuntime::create` receives the manifest and `EncodedRuntimeTemplate`, and
   is responsible for the hash closure, content hashing, and PNG decode validation.
8. On success it returns a `LoadedRuntime` containing only a valid `RecognitionRuntime`; this type
   has no observable half-initialized state.

Thus the manifest is the read authority for the runtime. The current path does not read
`project.toml`, nor scan directories to guess assets; this keeps "what the publication commit point
points to" consistent with "what the runtime actually loads".

### Sessions and Observation

The public runtime surface lives in `modules/engine/source/engine/session.hpp`:

- `EngineSessionConfig` fixes the live `ProjectFingerprint`, the pixel comparison budget per
  recognition, the recognition timeout, the maximum action frame age, and a shared
  `std::stop_token`.
- `EngineSession::create` requires all three ports to be non-null, stores the `LoadedRuntime` and
  the config, and first emits `SessionStarted`. If the first trace write fails, the session is not
  created successfully.
- `EngineSession::observe() -> Result<Observation>` first checks cancellation, then rechecks the
  target instance, captures, builds a lease via `ObservationLease::forFrame`, extracts
  `annotation::FrameIdentity`, and only after emitting `Observed` hands the handle to the caller.
- `EngineSession::resolvePage(Observation const&)` calls its
  `RecognitionRuntime::evaluatePage` against the frame held by the supplied observation and returns
  a `PageOutcome` composed of `ResolvedPage`, `UnknownPage`, or `AmbiguousPages`.
- `EngineSession::findAction(Observation const&, RecognizerId)` calls
  `evaluateActionTarget` against that same frame. A miss is a successful empty value of
  `Result<std::optional<ActionFound>>`, corresponding to D4 Tier A, and is not an error.
- `ActionFound` stores the original `AnchorEvidence`, an `ActionDetection` bound to the recognizer
  identity, and a deterministic `PixelPoint`. The click point is decided by annotation's
  `resolveClickPixel`; the match rect becomes an authorization-ready `Detection` through
  `pixelRectToFrameRect`.
- `EngineSession::act(Observation&&, ResolvedPage const&, ActionFound const&)` consumes the
  observation and on success returns an `ActReceipt` recording the authorized `FrameId` and the
  client-space coordinate.
- `EngineSession::waitForPage(PageId, timeout, pollInterval)` repeats observe/resolve, and on a hit
  returns a paired `Observation` and same-frame `ResolvedPage`; the caller need not, and should not,
  recognize again.

One product CLI data flow can be read directly in `entry/cli/run-windows.cpp`:

```text
loadRuntimeProject
  -> resolve page/action names
  -> bind WgcCaptureSession + DeliveryTarget
  -> create IFrameSource/IActionSink/ITraceSink adapters
  -> EngineSession::create
  -> waitForPage
  -> EngineSession::findAction(PageWait::m_observation, actionId)
  -> EngineSession::act
  -> IActionSink::click
```

The critical ordering inside `act` is:

1. cancellation gate;
2. session provenance and stale-handle guard;
3. `annotation::authorizeCoordinateAction`;
4. emit `ActionAuthorized`;
5. pixel → frame → client coordinate transform;
6. delivery-edge `IFrameSource::validateTargetInstance`;
7. `IActionSink::click(clientPoint, observation.m_lease)`;
8. immediately set `observation.m_invalidated = true`;
9. emit `ClickDelivered` and `ObservationInvalidated`.

Step 8 must come before the two fallible post-click traces. If the click has already landed but the
trace then fails, the caller may still hold the named rvalue alias that was passed in; invalidating
first guarantees that a retry gets `StaleObservation` and does not double-click because logging
failed.

### Trace Events

The schema is owned by `modules/trace/source/trace/event.hpp` under the id `umbraflow-trace/v1`;
engine and task write into the same stream. The events engine emits are:

- `engine.observed`, `engine.observation_invalidated`.
- `engine.page_resolved`, with outcome `Resolved` / `Unknown` / `Ambiguous` / `Stopped` / `Failed`.
  A completed attempt also carries `pageScores`: one entry per evaluated page naming its candidacy
  and the required anchor that scored worst against its own ceiling, so a non-resolution says how
  far off it was rather than only that it happened.
- `engine.action_found`, with outcome `Found` / `Absent` / `Stopped` / `Failed`.
- `engine.action_authorized`, `engine.action_rejected`, `engine.action_delivered`.

`engine-trace/v1`'s `PageResolved` / `PageUnknown` / `PageAmbiguous` and `ActionFound` /
`ActionAbsent` collapse into the outcome of the two kinds above. The stage-independent
`RecognitionStopped` and `Failure` become outcomes of the stage they occurred in, so a reader can
now tell *which* step stopped or failed — information the old vocabulary did not carry.
On the script path `SessionStarted` has no successor: the composition root's `run.started` records
the same instant and adds the project, task, source hash, framework version and bundle hash, Luau
compiler version, seed, and run identity. `entry/cli`'s smoke path writes no run-level event at all
and its trace opens on the first `engine.observed`; that path is deleted with stage 1d's TaskHost.

`trace::TraceRecorder` stamps `seq`, `runId` and `generationId` onto every event, plus `wallClock`
inside `meta`. `meta` is the documented non-golden field set, stripped by
`trace::stripNonGoldenFields` before a golden comparison. Engine does not own a sink: it borrows the
run's recorder, and opening the file, writing each line and flushing belong to `FileTraceSink` in
`modules/trace`.

## Constraints That Must Remain True

### Fail-closed

All conditions that would make "what to do where" uncertain fail toward rejection:

- When the session is missing any port, `EngineSession::create` returns `InvalidResource`.
- When the live fingerprint differs from the catalog fingerprint, recognition or authorization
  rejects.
- The `CaptureSessionId`, `TargetGeneration`, and `FrameId` of the page evidence, action detection, and
  delivery must be identical; the action recognizer must additionally belong to the active catalog,
  be of type `ActionTarget`, and be allowed on the resolved page.
- `ObservationLease::validate` validates session, generation, frame, and expiration; any mismatch
  returns `StaleObservation`.
- A recognition budget, deadline, or cancellation produces an explicit stop reason rather than
  treating a half-completed search as a miss. A page/action stop first emits `RecognitionStopped`,
  then maps to `RecognitionFailed`, `Timeout`, or `Cancelled`.
- `UnknownPage` and `AmbiguousPages` cannot masquerade as `ResolvedPage`; the parameter type of
  `act` itself requires a real `ResolvedPage`. An action miss, on the other hand, is an empty
  optional, keeping the Tier A normal control flow.
- `validateTargetInstance` is called again before delivery. When the target has been replaced, it
  emits `ActionRejected` and does not call the sink.

There are two layers of lease defense here, but with different responsibilities. The annotation
authorization layer compares the full frame identity and the expiration time; the current
controller's `checkPointerPreconditions` compares session, target generation, and age again before
the post, and validates that the coordinate is finite and falls within the client area. The
controller currently has no "latest FrameId" input, so it cannot be described as comparing the
current frame again at the delivery layer.

### Model B Ownership and Lifetime

D1's Model B is encoded as a handle rather than relying on calling conventions alone:

- `Observation` exclusively owns a `Frame`, its corresponding `ObservationLease`, and a
  `FrameIdentity`.
- It is non-copyable and only movable; both the move constructor and move assignment immediately
  mark the source as invalidated, so a moved-from handle behaves identically to a consumed handle.
- `EngineSession::resolvePage` and `EngineSession::findAction` first check the observation's
  invalidated flag; after invalidation, either query returns `StaleObservation`.
- `act` takes `Observation&&` and invalidates the entire observation after a successful delivery.
  The caller must `observe` again, structurally maintaining "one observation, multiple queries on
  the same frame, one coordinate action, observe again".
- The observation stores no pointer or borrow to `EngineSession`. Instead, it shares a private,
  immutable identity token with the session that vended it. The token follows a moved session, so
  existing observations remain valid after that move; using one with another session returns
  `InternalInvariant` without dereferencing a moved-from object.
- The session exclusively owns the runtime and the three ports; `ActionFound`, `PageWait`, and
  `ActReceipt` are all values that clearly own their results and do not return a dangling temporary
  view.

### Determinism and Bounded Execution

All queries on the same observation read the same frame, avoiding a game state change between two
implicit captures. The recognition policy is constructed each time from a fixed comparison budget, a
monotonic deadline, and a stop token; over-budget/timeout is a distinguishable stop and does not
treat a partial search result as a complete result.

The manifest order drives asset reading, duplicate hashes are deduplicated with the linear
`loadedHashes`, and this does not depend on the iteration order of an unordered container. The click
pixel is determined by annotation rules, and the coordinate transform comes from the
`CoordinateTransform` that the captured frame carries. The trace field order and wire names are also
fixed, which makes golden comparison easy and lets downstream reject unknown schemas.

The wall clock participates only in the recognition deadline, the lease staleness fuse, and the wait
deadline. Deviations on these paths only shorten the actionable window or return a stop/timeout, and
never turn an unknown state into an allowed action.

`waitForPage`'s sleep is sliced by `k_maxPollSleepSlice = 100ms`. Even if the caller provides a
10-second poll interval, cancellation and the deadline are checked between each slice, so a wait
does not tie the cancellation response to a full poll interval. `sweepKnownPopups` is called once at
the start of each round, but is currently an explicit no-op.

### Tracing at the Failure Site

D4 requires that an error be recorded at the instant it propagates to the upper layer, rather than
expecting a future script to record it voluntarily. The current concrete mechanism is not a global
exception hook but an explicit emit at the engine failure site:

- When `evaluatePage` / `evaluateActionTarget` returns an error, it first assembles a `Failure`
  carrying the frame identity, `errorKind`, and message, and only returns the original error after
  the emit succeeds.
- A recognition stop first emits `RecognitionStopped`, then returns the mapped structured error.
- When authorization or delivery-edge revalidation rejects, it first emits `ActionRejected`, then
  propagates the original error.

The emit itself can fail; `UF_TRY` propagates that failure immediately, so a trace infrastructure
failure is not downgraded into silent operation. Note also the current coverage: loader errors
before session creation, the pre-condition cancellation of observe/act, `waitForPage`'s own
timeout/cancellation, and a direct failure of `IActionSink::click` currently do not uniformly
generate a `Failure` event. When extending error paths, you must read the specific emit site and
cannot assume a central interceptor exists.

### Strict-Background

The engine only expresses the port contract that "must be strictly background"; the real mechanism
lives downstream in the Windows adapter:

- `modules/controller/source/controller/input.hpp` explicitly lists `SetForegroundWindow`,
  `SetFocus`, `SendInput`, `mouse_event`, `keybd_event`, and `SetCursorPos` as forbidden background
  APIs.
- `modules/controller/source/controller/platform/windows-input.cpp` calls `PostMessageW` only to a
  single resolved HWND, and rejects null and `HWND_BROADCAST`.
- The controller checks the lease, the target window's liveness, the client bounds, and the message
  encoding before the post; `ControllerActionSink` maintains held-input bookkeeping and compensates
  with a release when the click fails.

Thus the engine's platform-freedom does not reduce safety but layers the portable authorization
timing apart from the non-portable delivery proof. Any new adapter must re-honor `IActionSink`'s
strict-background and lease pass-through contract; merely "implementing the virtual function" does
not automatically earn that guarantee.

## Ports and Dependencies

The main inbound edges are as follows:

- `entry/cli/run-windows.cpp` provides the selected target, the live fingerprint, the budget, the
  timeout, cancellation, and the three ports, then drives `waitForPage -> findAction -> act`.
- annotation provides the catalog, templates, page signatures, action-target definitions, and the
  allowed-page policy through the runtime manifest.
- domain provides `Frame`, `CoordinateTransform`, `Detection`, `ObservationLease`, `CaptureSessionId`,
  `TargetGeneration`, `FrameId`, and `AutomationErrorKind`.
- core provides `Result`/`Status`, monotonic time, integer types, and contracts.

The main outbound edges are as follows:

- To annotation: it calls `parseRuntimeManifest`, `RecognitionRuntime::create`, `evaluatePage`,
  `evaluateActionTarget`, `resolveClickPixel`, `ActionDetection::create`, and
  `authorizeCoordinateAction`.
- To the capture adapter: it requests a target recheck and capture before observe, and rechecks
  again at the final edge of act.
- To the action adapter: only the already-transformed `Point<ClientSpace>` and the lease with no
  dropped fields cross over.
- To the trace adapter: it sends structured `TraceEvent` synchronously; the file path,
  append/truncate, and flush do not cross the engine boundary.
- To the caller: it returns a structured page outcome, an optional action, an act receipt, or an
  `Error` that preserves `AutomationErrorKind`, and does not bring platform exceptions or Luau types
  into the API.

The dependency direction must remain unidirectional: entry can see both engine and controller, but
the engine cannot include controller in reverse. Otherwise fake ports cannot substitute for desktop
capability in platform-independent tests, and Windows types would leak into the stable domain
surface of a future Luau binding.

## Tests

`tests/engine/test-runtime-loader.cpp` pins the read boundaries:

- Compiles an authoring fixture, writes out the publication directory, then has `loadRuntimeProject`
  read it back and actually recognize pages, verifying the compile → publish → load → recognize
  loop.
- A corrupt manifest returns `InvalidResource`.
- A missing template returns `IoFailure`, with error text containing `assets/templates`.
- Tampered template bytes cause a hash mismatch and return `InvalidResource`.
- A manifest exceeding 16 MiB is rejected by the stat fast path before reading.

`tests/engine/test-trace.cpp` pins the wire contract:

- The schema-first fixed order and exact golden JSON of a full-field event.
- A minimal event outputs only `schema` and `kind`.
- JSON escaping of quotes, backslashes, and control bytes.

`tests/cli/test-file-trace-sink.cpp` pins the JSONL transport:

- Each emit produces one line as defined by `serializeTraceEvent`.
- An unopenable path returns an error `Status` rather than silently dropping the trace.

`tests/engine/test-session.cpp` uses `FakeFrameSource`, `CountingActionSink`, and
`CollectingTraceSink` to pin the runtime state machine:

- The happy path delivers only once, the lease's `FrameId` reaches the sink unchanged, and the event
  order is exactly start/observe/page/action/authorize/click/invalidate.
- A fingerprint mismatch, a wrong allowed page, and an expired lease all result in zero delivery and
  leave the corresponding failure/rejection trace.
- Reuse after an action and reuse of a moved-from handle return `StaleObservation`; a foreign
  session handle returns `InternalInvariant`.
- When a `ClickDelivered` trace failure occurs after the real click, the handle has already been
  invalidated, and a retry does not double-deliver.
- An unknown page stays `UnknownPage` and does not produce a `ResolvedPage` usable by `act`.
- A comparison budget stop and a recognition cancellation preserve different error kinds and record
  `RecognitionStopped`.
- Cancellation before observe and act cancellation after observe are both zero delivery.
- When the target instance becomes invalid after observation, the delivery-edge guard blocks the
  sink call.
- A mid-way cancellation during a 10-second poll sleep can return within the test's upper bound
  thanks to the 100 ms slicing; there are also tests for an immediate timeout and a successful hit
  on the second frame.

These tests deliberately separate the engine's ordering from Windows: the engine fakes pin the lease
pass-through, the CLI sink tests pin the JSONL durability, and the controller's lease, window
identity, message encoding, and strict-background constraints are pinned by `tests/controller/`. The
compensation and real composition of `ControllerActionSink` currently have no separate CLI unit
test, so when modifying that adapter you must not mistake downstream tests for direct coverage.

## Future Extensions

### B2 Luau Binding

`docs/plans/2026-07-23-engine-architecture.md` specifies that the current API mirrors the locked D1
Model B, precisely so that B2 can bind without refactoring the domain surface:

- The Luau capture/observe handle corresponds to the move-only `Observation`.
- Same-frame page query/find corresponds to `resolvePage` and `findAction`.
- A Tier A miss corresponds to a successful empty optional; the Luau expression of Tier B/C must
  still be implemented per D4 and the hardening ledger, and cannot simply turn every C++ `Error`
  into a plain error that `pcall` can swallow.
- click corresponds to `act`, and the whole handle is invalidated on success.
- wait corresponds to `waitForPage`, and directly carries the observation that hit it.

There is currently no manifest edge between `modules/script` and `modules/engine`, and B2 is not yet
implemented. The binding layer should depend on the stable engine operation surface and should not
push `lua_State`, userdata, or scheduler concepts back into the engine. D4 Tier C's non-swallowable
cancellation, VM interrupt, instruction/runtime budget, and sandbox are still constrained in their
implementation by `docs/plans/2026-07-21-p0b-luau-hardening-ledger.md`.

### Platforms and Fakes

`IFrameSource`, `IActionSink`, and `ITraceSink` are the formal seams for the P3 second platform and for
test fakes. When adding a platform, target discovery and the adapter still go in entry/platform; the
engine adds no `#ifdef Windows`. A new `IActionSink` must prove the target instance, lease fencing,
and strict-background, not merely transport coordinates.

### Waiting and D6

`EngineSession::sweepKnownPopups` already sits at the start of every `waitForPage` cycle.
`docs/plans/2026-07-23-engine-architecture.md` places P0-C's minimal known-popup sweep and P1's
`bot:on` registry here. The current no-op only locks in the call timing and does not mean a popup
policy already exists; a future implementation must also obey grill D6's cycle boundary,
declaration-order first-match, and bounded hits.

### Lifetime and D10

The current `EngineSession` is the owner of a single run, with no task id, queue, or concurrency.
D10 reserves `load_project/start_task/pause/resume/cancel/query_task/subscribe_events` in
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md`, and requires that the API semantics be
preserved when upgrading from P0's one-run-at-a-time to P2's resident Engine. The extension point
should manage the lifetime outside the session and should not weaken the observation's single-frame
and single-action invariants.

### Runtime and Trace Evolution

`docs/plans/2026-07-23-engine-architecture.md` explicitly defers reading the project-level
fingerprint from `project.toml`; the current authority is the fingerprint embedded in the runtime
manifest. When reading is added in the future, the consistency check between the two authorities
should be made explicit, and one of them must not be chosen silently.

Trace evolution should add a new schema version and keep `TraceEvent`, the explicit wire-name
switch, the serializer golden tests, and all sinks/consumers in sync. It must not sneakily change v1
via a C++ enum rename; nor introduce unordered iteration of new fields into the stable output.

When adding swipe, key, or richer actions, one should extend an explicit action port/receipt and the
corresponding lease rules, rather than bypassing `act` to expose controller directly. Regardless of
the action kind, delivery-edge revalidation, invalidate-before-fallible-post-delivery-work, zero
focus stealing, and diagnosable trace remain the shapes the seam must preserve.
