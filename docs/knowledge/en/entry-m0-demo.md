# entry/m0-demo: the frozen M0 on-hardware substrate

`entry/m0-demo` is a frozen Windows acceptance program. It preserves the shortest path verified on
a real machine against a high-integrity target window: WGC background capture, grayscale SAD
template matching, client-coordinate clicking based on an observation lease, strict-background
`PostMessageW` delivery, foreground and cursor guards, delivery audit, input compensation after
failure, and orderly shutdown.

The word "frozen" in this document is defined by `docs/plans/2026-07-20-m0-demo-port-deviations.md`
and `docs/plans/2026-07-23-engine-architecture.md`: do not keep adding product capabilities to this
directory, and do not let new product entries link its implementation. When reuse is needed, copy
the already-verified safety semantics into an `engine`/runner adapter and carry them again under a
product contract.

## What It Verifies

The M0 demo has three process entry points, dispatched from `entry/m0-demo/main.cpp`.

- The default entry parses `Args`, selects a real window, creates a `WgcCaptureSession` and a
  `DeliveryTarget`, loads the three templates `home`, `result`, and `reset`, and then calls
  `runPipeline`.
- The `capture` subcommand parses `CaptureArgs` and only performs target discovery, WGC capture, and
  PNG output; it is implemented as `runCapture` in `entry/m0-demo/capture-mode.cpp`.
- The `input-agent` subcommand parses `InputAgentArgs`, runs resident and polls an append-only JSONL
  command file, and executes `capture`, `click`, and `quit`; it is implemented as `runInputAgent` in
  `entry/m0-demo/input-agent.cpp`.

The business shape of the default pipeline is fixed:

```text
click home -> wait result -> click reset -> wait home
```

It is only responsible for proving that this finite loop can execute safely in the background.
`LoopConfig::m_loops` determines the repeat count, and `RunSummary` aggregates attempted, succeeded,
guard violation, stop, and audit clean. `RunSummary::passed()` returns true only when it was not
stopped, the audit is clean, there is no guard violation, and every attempt succeeded.

It deliberately does not own the following product responsibilities.

- It does not read `project.toml`, `annotations.toml`, or
  `generated/annotations.runtime.toml`.
- It does not create or call annotation's `RecognitionRuntime`, and it does not understand
  element IDs, capability sets, page signatures, `ResolvedPage`, or page references. (Corrected
  2026-07-31: this listed `annotation_type` and `allowed_page_ids`, both removed by
  [the capability plan](../../plans/2026-07-31-annotation-model-capabilities.md). The point of the
  bullet is unchanged and stronger — m0-demo understands none of the annotation model, whatever
  shape it takes.)
- It does not perform "page-evidence-authorized actions." As soon as a template passes the SAD
  threshold, `clickWhenPresent` clicks the center of the matched rectangle.
- It does not provide Luau tasks, a long-lived Engine lifecycle, a generic popup sweep,
  cross-platform ports, or a GUI.
- It does not treat the `capture` subcommand as input authorization; it is merely a capture
  diagnostic tool.
- It does not treat the `input-agent` file protocol as product IPC. It is an acceptance fixture in
  which, after a single UAC prompt, an unelevated driver side continuously operates an elevated
  agent.

So its bypass of the annotation authorization stack is not an omission but a historical stage
boundary. M0 first proves that the underlying capture/input substrate is viable on real hardware;
only after S0 are page evidence, action capability, manifest closure, and fail-closed authorization
defined as the product contract. `docs/plans/2026-07-22-annotation-design.md` explicitly requires
that `Detection + ResolvedPage + ObservationLease + target fingerprint` jointly authorize an action,
whereas M0 has only template hits, leases, and target revalidation, without the `ResolvedPage`
capability layer.

The on-hardware acceptance record lives in `docs/TODO.md` §0,
`docs/plans/2026-07-20-post-port-win32-robustness.md`, and the archived
`docs/archive/plans/2026-07-20-ui-verification-runbook.md`. The 2026-07-21 result was:

- the 卡厄斯梦境 client area was 1600×900, and the K2 crop between the WGC frame and the client had
  `delta=(0,0)`;
- the avatar was switched three times and the tab was switched three times, both successfully;
- the first-time "potential" introduction overlay was recognized and safely closed;
- all delivered clicks went through the strict-background `PostMessage` path without grabbing the
  foreground;
- on the real machine `StaleObservation: lease expired` was indeed triggered and kept
  `delivered:false`, proving that an expired observation fails closed.

Here `delta` is `frame.width/height - client.width/height`. The capture results in
`capture-mode.cpp` and `input-agent.cpp` both record the size difference by this definition.
`delta=(0,0)` is an acceptance fact for that target and that machine configuration, not a universal
guarantee for all WGC targets.

## Execution Flow

### Entry arguments and composition

`entry/m0-demo/args.hpp` defines three groups of value types.

- `SelectorArgs` holds an optional PID, HWND, window class, and title.
- `Args` holds three template/ROI groups, `m_threshold`, `Mode`, the loop count, the maximum frame
  age, the capture stall timeout, an optional click delay, the seed, and the log path.
- `CaptureArgs` holds the selector, the output path, the frame count, the interval, and the log path.
- `InputAgentArgs` holds the HWND, the queue/results files, the output directory, and the idle
  timeout.

The default pipeline's `--threshold` is required and ranges over 0..255; the default
`--max-action-frame-age` is 750 ms and the default `--stall-timeout` is 1000 ms. `Mode` defaults to
`Guard`, and the other value is `Coexist`. These constraints are enforced by `parseArguments` in
`entry/m0-demo/args.cpp`, while the public data shape lives in `entry/m0-demo/args.hpp`.

`runWithLog` in `main.cpp` is the composition root of the default path:

1. `ensurePerMonitorAwareV2()` declares DPI awareness.
2. `installConsoleControlHandler()` installs the Ctrl-C/Ctrl-Break stop flag.
3. `enumerateCandidates()`, `buildSelector()`, and `resolveTarget()` produce a
   `ResolvedTarget`.
4. Creates a `WgcCaptureSession` with a fixed `CaptureSessionId{1}`.
5. `loadTemplate()` converts the three PNGs into grayscale `Template`s.
6. Assembles `Templates` and `LoopConfig` from `Args`.
7. Creates a `DeliveryTarget` from the HWND, session, generation, and client size.
8. Calls `runPipeline()`, and finally closes the console control registration.

In `entry/m0-demo/pipeline.hpp`, `Template` owns a label, grayscale pixels as a
`std::vector<std::byte>`, a width and height, and a `Rect<FrameSpace>` search ROI. `Templates` is
merely the fixed home/result/reset triple; it is not a generic recognizer collection.

### Capture, SAD Matching, and Result Acceptance

The real recognition chain of the main loop is in `entry/m0-demo/pipeline.cpp`.

`captureFresh()` first calls `Machine::ensureTargetUnchanged()`, then `WgcCaptureSession::capture()`,
and revalidates the target again after a successful capture. Only
`AutomationErrorKind::CaptureStalled` is interpreted as "no frame this round" and allowed to keep
waiting; other capture errors terminate as-is. This prevents `StaleObservation` from being mislabeled
as an ordinary stall.

`recognizeRaw()` performs the following for each frame:

```text
Frame
  -> CoordinateTransform::frameRectToPixelRect(template.m_roi)
  -> image::cropBgra8
  -> bgra8ToGray8
  -> GrayImage::create
  -> matchTemplateSad
  -> optional<SadMatch>
```

The SAD search's comparison budget is fixed at 64 Mi pixel comparisons. `SadSearchPoll` checks the
global stop flag and the transition timeout during the search. `SadSearchStopReason::Cancelled`,
`TimedOut`, and `ComparisonBudgetExhausted` become stopped, timed out, and failed respectively; they
are never downgraded to "no match."

The coordinates returned by `matchTemplateSad` are relative to the ROI; `recognizeRaw()` adds the
ROI's x/y back with checked addition to obtain a frame-space `SadMatch`. Then
`acceptMatch(found, width, height, maximumAverageSad)` computes:

```text
area   = width * height
budget = maximumAverageSad * area
accept = found exists && area != 0 && found.score <= budget
```

On multiplication overflow, `budget` saturates to the `uint64` maximum; the normal CLI path has
already bounded the per-pixel average threshold to 0..255, and the template size has passed load-time
and ROI checks. The equals case belongs to the acceptance boundary.

This threshold scheme is not the product's basis-point model. S0 defines the following in
`docs/plans/2026-07-22-annotation-design.md` §1.4:

```text
maxSad = floor((10000 - minSimilarityBp) * 255 * templatePixels / 10000)
hit    = sadScore <= maxSad
```

The product stores `min_similarity_bp` in 0..10000, where a higher value demands greater similarity;
M0 stores `maximumAverageSad` in 0..255, where a higher value is more permissive. Not only are the
two opposite in unit and direction, but S0 also mandates integer floor, schema validation, and
manifest persistence. Therefore the engine recognizes only basis points, and
`docs/plans/2026-07-23-engine-architecture.md` explicitly mandates not migrating M0's recorded
`--threshold` value.

### From a Match to a Background Click

Once a match is accepted, `hitCenterFrame()` constructs a `Rect<FrameSpace>` from the `SadMatch`'s
top-left corner and the template's width and height, and takes the geometric center.
`CoordinateTransform::frameToClient()` then converts that center into a `Point<ClientSpace>`.

`ObservationLease::forFrame(*captured, config.m_maxActionFrameAge)` binds the frame's session, target
generation, and expiry instant into a delivery credential. Before the click,
`Machine::ensureTargetUnchanged()` again revalidates the target instance and runs
`ResolvedTarget::revalidate()`, and then calls the controller's:

```text
click(DeliveryTarget, ObservationLease, Point<ClientSpace>, HeldInputs, AuditLog)
```

The controller rechecks the lease session, expiry, generation, coordinate finiteness, client bounds,
and the Win32 signed-16-bit encodable range at the delivery edge. Any inconsistency returns
`StaleObservation` or `ActionRejected` from `checkPointerPreconditions()` in
`modules/controller/source/controller/input-revalidation.cpp`, and never calls platform delivery.

The `click()` pointer sequence is `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`, `WM_LBUTTONUP`; these are
ultimately queued by `PostMessageW` in
`modules/controller/source/controller/platform/windows-input.cpp`. M0 never calls
`SetForegroundWindow`, `SetFocus`, `SendInput`, `mouse_event`, `keybd_event`, or `SetCursorPos`.

After a successful click, the returned `deliveredAt` becomes the causal barrier for the next page
observation. `waitUntilPresent()` uses `frameIsCausal(capturedAt, deliveredAt)` to discard frames
earlier than the click's completion, preventing a stale image from being treated as evidence of a
state transition.

Click failures are classified by `clickFailureDisposition()`:

- `StaleObservation`: records `click_retry` and recaptures;
- `ControllerDisconnected`: cannot queue, so it aborts the entire run;
- other errors: immediately fail the current step and preserve the real error kind, avoiding
  disguising it as a timeout after repeated retries.

### State Guards, Audit, and Shutdown

`GuardPolicy::forMode(Mode::Guard)` in `entry/m0-demo/guard.hpp` requires comparing the foreground and
the cursor. At the start of each round, `runOne()` obtains a `GuardBaseline`: the baseline foreground
must be non-empty and not the target; at the end of each round it observes again, and the foreground
and cursor must equal the baseline. `Mode::Coexist` disables both comparisons.

The point of the guard is to verify that "background automation did not change the user's global
interaction state," not to authorize actions. Even if a business step fails, `combineLoopStatus()`
still preserves a guard violation that happened at the same time, so that one failure does not mask
another.

Every controller delivery enters the `AuditLog`. At shutdown, `summarizeAudit()` requires that every
recorded HWND equals the target and that the message belongs to the allowlist in
`entry/m0-demo/platform/windows-background-messages.cpp`: mouse move/down/up, key down/up, `WM_CHAR`,
and `WM_UNICHAR`.

`runPipeline()` gathers the mutable run state into a private `Machine`: `ResolvedTarget`, a move-only
`WgcCaptureSession`, `DeliveryTarget`, `HeldInputs`, and `AuditLog`. Regardless of whether the main
flow succeeded, `shutdownMachine()` proceeds in order:

```text
releaseHeld -> session.close -> audit/summary -> log.flush
```

A failure in an earlier stage does not prevent later stages from running, and the first error is
ultimately preserved. This guarantees that a partial Down does not skip a best-effort Up because of
another error, and that the audit and log are persisted as much as possible.

### Elevated input-agent split-process protocol

When the on-hardware target's integrity is higher than an ordinary driver process, acceptance uses a
split-process approach: the developer manually launches `m0-demo input-agent` once via
`Start-Process -Verb RunAs`; thereafter the unelevated side only appends commands to the queue and
reads results. The elevation boundary is thus concentrated in a single long-lived agent, rather than
triggering UAC on every click.

`entry/m0-demo/input-agent-protocol.hpp` defines five variants:

- `InputAgentCaptureCommand{output}`;
- `InputAgentClickCommand{x, y, outputBefore, outputAfter, settle}`;
- `InputAgentKeyCommand{key, outputBefore, outputAfter, settle}`;
- `InputAgentScrollCommand{x, y, delta, outputBefore, outputAfter, settle}`;
- `InputAgentQuitCommand`.

The corresponding JSON objects accept only `op=capture|click|key|scroll|quit` and each one's exact
field set. `parseInputAgentCommand()` rejects anything over 64 KiB, invalid UTF-8, duplicate/unknown
fields, malformed JSON numbers, empty paths, NUL, non-finite coordinates, and a settle over 5000 ms.
The default action settle is 400 ms.

`delta` is a whole number of wheel notches rather than raw `WHEEL_DELTA` units, and the parser
resolves it through `WheelDelta::create`, so a zero, a fraction, and a count whose raw form would
not fit `wParam`'s signed 16-bit word are all refused before any command is queued — the same way
`key` is resolved through `KeyInput::fromName`.

`InputAgentQueueReader` reads the append-only queue incrementally by offset, accepts LF/CRLF, and
retains incomplete lines; it fails closed when the queue is truncated or a pending command exceeds
1 MiB. `ResultWriter` calls `flushDurably()` after each JSONL result it writes.

The agent's file-permission surface is likewise part of the protocol: the queue and results must be
different and both outside the output directory; screenshot paths must be constrained inside the
output directory by a confinement check and must not alias the IPC files.
`platform::FileWriter::createExclusive()` performs a relative `NtCreateFile(FILE_CREATE)` through a
chain of already-verified, kept-open directory handles, rejecting overwrite, reparse escape,
alternate data streams, and directory-rename races.

The observe -> act hot path of `executeClick()` is, and `executeScroll()` runs the same one with
`scroll` in place of `click`:

```text
reserve fresh before/after outputs
-> capture immutable before Frame
-> ObservationLease::forFrame
-> validateInputAgentPointerAction
-> ResolvedTarget::revalidate
-> requireUnchangedTarget
-> WgcCaptureSession::validateTargetInstance
-> click
-> encode/write before PNG
-> settle
-> capture/encode/write after PNG
```

The encoding and durable flush of the before PNG are deliberately moved to after the click. On real
hardware it was found that placing the 1600×900 BGRA encode, disk write, and `FlushFileBuffers`
between capture and click consumed the 750 ms lease budget and produced unexpected expiry. After the
move, the same immutable pre-click `Frame` is still retained, but forensic I/O no longer lengthens
observe -> act.

A command parse error writes a failure result and then continues; a target revalidation/instance
failure writes a result and stops the agent. After each command completes,
`clearInputAgentCommandAudit()` clears the audit to prevent the resident agent's records from growing
without bound.

## Constraints That Must Remain True

**Fail closed.** No frame, a search interrupted by a control signal, an exhausted comparison budget,
an invalid ROI/template, a changed target generation, an unconfirmable instance, an expired lease,
out-of-bounds coordinates, an out-of-bounds background message, or an unclean audit — none of these
may be interpreted as a successful delivery. The concrete mechanisms are distributed across
`captureFresh()`, `searchStopStatus()`, `requireUnchangedTarget()`, `ObservationLease`, the
controller's `checkPointerPreconditions()`, `checkGuard()`, and `summarizeAudit()`. In particular,
`CaptureStalled` is the only retryable capture absence, and unknown errors do not enter a permissive
fallback.

**Strict background.** Input goes only through the controller's window-targeted message delivery; the
guard checks that the foreground/cursor did not change, and the audit further checks the target HWND
and the message allowlist. This forms three layers of evidence: "delivery mechanism + external state
observation + after-the-fact recording." `Coexist` disables the guard comparisons, so acceptance that
needs to prove the strict-background property must use the default `Guard` and cannot rely solely on
the click returning success.

**Observation and action are causally consistent.** A click must carry an `ObservationLease` produced
by the same captured frame; the session, generation, and age are revalidated at the delivery edge.
Recognition after the action must also satisfy `capturedAt >= deliveredAt`. Therefore a stale frame
can neither authorize the current click nor prove that the click has already caused a page
transition.

**Target identity is continuous.** The target is revalidated before and after capture, before a
click, and before an input-agent click. `GenerationBumped` and `InstanceUnconfirmed` become
`StaleObservation`; `Lost` becomes `ControllerDisconnected`. If shutdown cannot confirm the original
target, it constructs a poisoned session identity so that compensating delivery is rejected at the
controller precondition, rather than risking sending an Up to a possibly-reused HWND.

**Deterministic decisions.** Given the same grayscale frame, template, ROI, and threshold, the SAD
acceptance boundary is fully determined by the integer score and integer budget. Optional click
pacing uses `SplitMix64` and an explicit seed, and a fixed seed produces a fixed delay sequence. Real
WGC arrival times and OS scheduling are themselves nondeterministic; what the design guarantees is
that once these inputs are given, the branching rules do not depend on floating-point confidence or a
hidden random source.

**Ownership and lifetime are visible.** `Template` owns its grayscale bytes; `Machine` owns the
capture session, held state, and audit; `InputAgentQueueReader` owns the path, offset, and pending
buffer; the Windows `FileWriter` exclusively owns native handles via a `unique_ptr<State>`. The
`Frame` value is retained across a click in the input-agent, deferring encoding using the same
immutable frame; `ObservationLease` is a value credential and does not store a raw reference to the
caller. RAII handle wrappers are responsible for closing Win32 process/token/file handles.

**Bounded waiting and stoppability.** The transition timeout, capture stall timeout, SAD comparison
budget, input-agent settle cap, idle timeout, and queue/command byte limit all have explicit bounds.
The console handler sets a lock-free atomic only on Ctrl-C/Ctrl-Break; the pipeline and the SAD poll
cooperatively check it. Windows close/logoff/shutdown are not within this handler's coverage, which is
the frozen deviation recorded as F-19 in `docs/plans/2026-07-20-m0-demo-port-deviations.md`.

## Relationship to Product Code

The inbound edge is the CLI and file assets. The caller provides a window selector, three trusted
PNGs, three frame-space ROIs, an average SAD threshold, and a run policy; the input-agent caller
provides an existing append-only queue, a results path, and a confined output directory.

The outbound edge toward `controller` includes:

- discovery: `enumerateCandidates`, `resolveTarget`, `ResolvedTarget::revalidate`;
- DPI: `ensurePerMonitorAwareV2`;
- capture: `WgcCaptureSession`, `WgcCaptureOptions`, `Frame`;
- input: `DeliveryTarget`, `ObservationLease`, `click`, `releaseHeld`, `AuditLog`.

What crosses these edges is typed target identity, session/generation, client geometry, frame
timestamp/transform, lease, and delivery audit, rather than a raw HWND plus unconstrained
coordinates.

The edge toward `image`/`vision` is PNG decoding, BGRA crop, grayscale conversion, `GrayImage`, and
`matchTemplateSad`. M0 itself decides the ROI, comparison budget, timeout poll, and acceptance
threshold; vision only returns `SadSearchOutcome`/`SadMatch` and does not decide whether to authorize
a click.

The edge toward `domain`/`core` is `FrameSpace`, `ClientSpace`, `DesktopSpace`, `CoordinateTransform`,
`MonotonicInstant`, typed IDs, checked arithmetic, and `Result<...>`/`AutomationErrorKind`. These
types keep spatial, temporal, and identity errors from slipping silently through the pipeline as
ordinary integers.

The logging edge is carried by `JsonlLog` and `LogLine` in `entry/m0-demo/log-jsonl.cpp`. Each line
can carry elapsed time, loop index, frame ID, target generation, SAD score (the field is still named
`confidence`), lease outcome, and detail. A log write/flush failure is itself an `InvalidResource` and
is not silently lost. This schema is the diagnostic format of the frozen demo, not the engine's
product trace schema.

annotation/engine has no link edge with M0. The current product path is
`annotation -> engine -> entry/umbra-flow`. If the runner needs the target-poison, compensating-Up, or
elevated-agent semantics that M0 has proven, `docs/plans/2026-07-23-engine-architecture.md` requires
copying the semantics at the adapter layer rather than linking `entry/m0-demo`.

## Tests

`tests/CMakeLists.txt` composes the following files into `test-m0-demo`, linking
`${PROJECT_NAME}_m0_demo_support`.

- `tests/m0-demo/test-args.cpp` pins the full arguments, defaults, selector, duration, click
  delay/seed, threshold 0..255, capture arguments, and the rejection boundaries of the input-agent
  arguments.
- `tests/m0-demo/test-pipeline.cpp` pins click error triage, the match center, the inclusive boundary
  of the per-pixel-average threshold, target revalidation, the post-action frame causal barrier,
  guard/status combination, ROI/template geometry, `RunSummary::passed()`, and the target/message
  audit.
- `tests/m0-demo/test-guard.cpp` pins the Windows integrity RID label, the foreground/cursor
  comparison in Guard mode, the non-target baseline, and the disabling semantics of Coexist.
- `tests/m0-demo/test-input-agent.cpp` pins the strict JSON command grammar, UTF-8, the settle cap,
  path confinement, incremental line framing, queue truncation/size limit, handle-relative exclusive
  output, per-command audit clearing, client bounds, and stale-generation rejection.
- `tests/m0-demo/test-capture-mode.cpp` pins that the log path must not alias either output PNG.
- `tests/m0-demo/test-log-jsonl.cpp` pins the error kind/context/native origin, the JSONL field
  order/null representation, and sink open/write/flush failures.
- `tests/m0-demo/test-pacing.cpp` pins the delay range, inclusive sampling, fixed delay, and
  `SplitMix64` seed determinism.
- `tests/m0-demo/test-shutdown.cpp` pins the release -> close -> audit -> flush order and proves that
  an early error does not skip later cleanup.

These are pure behavioral and boundary tests, and are not equivalent to real Windows UI acceptance.
The K2 `delta=(0,0)`, not grabbing focus, the actual `PostMessage` effect on a high-integrity target,
the before/after image changes, and the lease's behavior under hardware latency all come from the
aforementioned 2026-07-21 runbook/TODO record. Conversely, on-hardware acceptance also cannot replace
the automated tests for the input-agent parser, path races, and shutdown ordering.

`docs/TODO.md` still lists occlusion, minimization/`CaptureStalled`, Ctrl-C during delivery, and
10–20 minute long-run verification as incomplete. Therefore these properties cannot be extrapolated
from the short-run scenarios that M0 has already passed.

## Retirement and Migration

M0 has no internal seam for "growing it into a product"; the correct way to extend it is to replace
it from the outside.

The recognition and authorization seams have already moved to `modules/annotation` and
`modules/engine`. New recognizers, page evidence, action targets, default clicks, basis-point
thresholds, and runtime manifests should all follow `docs/plans/2026-07-22-annotation-design.md`, and
must not add a fourth M0 template or stuff a page special case into `clickWhenPresent()`.

The platform-composition seam is in the product runner's `IFrameSource`/`IActionSink` adapter.
`docs/plans/2026-07-23-engine-architecture.md` requires the engine to stay platform-independent, with
Windows WGC and background input composed in the entry adapter; the Fake port replays synthetic frames
in CI and records zero/present delivery. A future second platform should also implement these ports
rather than conditionally compiling the M0 pipeline.

The elevation seam belongs to P0-C. The product plan currently allows `umbra-flow run` to elevate as a
whole first; if the on-hardware UIPI result requires a split-process, then copy the M0 input-agent's
protocol safety semantics into the runner adapter: append-only framing, a strict parser, output
confinement, fresh-file create, durable results, target/lease revalidation, and the agent stop policy.
The protocol may evolve, but the frozen demo does not evolve with it.

Follow-up work on capture robustness is grounded in
`docs/plans/2026-07-20-post-port-win32-robustness.md`, including occlusion, pairing the stall-timeout
with lease-age, capture wait cancellation, and so on. Do not locally build, inside M0, a lifecycle
that differs from the product port.

The retirement condition is not "the engine already has code" or "CI is green," but on-hardware
capability parity. The equivalence checkpoints given by `docs/plans/2026-07-23-engine-architecture.md`
are:

1. `umbra-flow run`, after being authorized by the product annotation/engine, clicks successfully in
   strict background on real hardware;
2. K2 capture is still `delta=(0,0)`;
3. an expired lease still fails closed with zero delivery;
4. the A1+B1 on-hardware end-to-end loop, from workbench-generated manifest to runner consumption, is
   complete;
5. the Fake IFrameSource and the static screenshot regression pin the same fail-closed semantics.

The frozen declaration in `docs/plans/2026-07-20-m0-demo-port-deviations.md` further requires: retain
M0 as an acceptance reference until the above on-hardware capability parity is reached, and retire it
only afterward. As of the current state of `docs/TODO.md`, the product on-hardware smoke and the
workbench -> manifest -> runner end-to-end are still pending developer execution, so this reference
cannot be deleted merely because the product path already exists.
