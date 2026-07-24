# entry/cli Architecture Knowledge

`entry/cli` is the command-line entry point for `umbra-flow` and the composition point on Windows.
It connects the platform-independent `engine` ports to the real capture and background-input
capabilities of `controller`, and owns arguments, resource names, and process exit codes.

## Entry-Point Responsibilities

This directory owns four kinds of product boundary.

The first is the process boundary. `entry/cli/main.cpp` copies `argv` into an owning
`std::vector<std::string>`, recognizes the `run` subcommand, emits usage, a success report, or an
error, and collapses every outcome into an integer exit code. With no subcommand it prints only
the project name and usage; there is currently no default behavior that implicitly runs a task.

The second is the command boundary. `RunArgs` in `entry/cli/args.hpp` is the complete value object
for one `run`, and `parseRunArguments` in `entry/cli/args.cpp` is responsible for turning ten
paired value flags into paths, names, budgets, and monotonic-clock durations. After a successful
parse there are no "unset" optional fields: the four required items must be non-empty, and the
remaining fields already carry safe defaults.

The third is the resource-name boundary. A published runtime manifest uses stable IDs, whereas a
human enters names when typing `--page` and `--action`. `entry/cli/name-resolution.hpp` provides
`resolvePageName` and `resolveActionName`, which perform the name-to-ID translation over an
already-validated `annotation::RecognitionCatalog`.

The fourth is the composition boundary. The Windows implementation, `runProduct` in
`entry/cli/run-windows.cpp`, selects a real window, creates the three adapters for capture, input,
and trace, and then wires them to `engine::EngineSession`. Non-Windows builds use
`entry/cli/run-unsupported.cpp`, which keeps the same product binary and testable command layer
but has `runProduct` explicitly return `UnsupportedCapability`.

This directory deliberately does not own the following responsibilities:

- It does not own recognition, page determination, action authorization, or the observation
  lifecycle; these belong to `modules/annotation/` and `modules/engine/`.
- It does not own Win32 window enumeration, WGC, target generation, lease validation, or message
  delivery; these belong to `modules/controller/`, and the CLI only does thin adaptation and
  assembly.
- It does not define a task language or a general workflow. The current `runProduct` is a fixed
  smoke flow: wait for one page, look for one action target, and click once when it exists.
- It does not read authoring documents, nor does it generate manifests or templates. It only
  consumes published projects that `engine::loadRuntimeProject` can load.
- It does not provide a foreground or global-input fallback. A background-delivery failure is a
  product failure.
- It does not take on a daemon, tray, scheduler, or multi-task lifecycle; the current design is
  one `run` per process.

This boundary explains why target discovery stays in the entry layer rather than moving into
`engine`: `engine::FrameSource` receives "one already-bound target" and thereby stays
platform-independent, while window-title selection, DPI declaration, and Win32 geometry are the
product's policy for how it obtains that port on Windows.

## Command Execution Flow

### Command-Line Entry Point

The `dispatch` in `entry/cli/main.cpp` has only two public product paths:

- Empty arguments: print `g_projectName` and `runUsageText()`, and return `0`.
- First argument is `run`: hand the remaining arguments to `dispatchRun`.

Any other first argument is treated as an unknown subcommand, prints an error and usage, and
returns `1`. `main` also checks that `argumentCount` can be safely converted and is non-zero, and
it catches `std::exception` and unknown exceptions at the outermost level; exceptions never cross
the process boundary.

The `RunArgs` in `entry/cli/args.hpp` corresponds to the following real CLI surface:

- `--project DIR`, `--selector TITLE-SUBSTRING`, `--page NAME`, and `--action NAME` are required.
- `--timeout SEC` defaults to 30 seconds and is the overall deadline for `waitForPage`.
- `--poll MS` defaults to 250 ms and accepts only 1 to 60000 ms.
- `--budget N` defaults to `1 << 28` and bounds the number of pixel comparisons in a single
  recognition.
- `--recognition-timeout MS` defaults to 2000 ms and is the deadline for each recognition.
- `--max-frame-age MS` defaults to 750 ms and determines how long an observation lease remains
  usable for an action.
- `--trace PATH` defaults to `umbra-flow-trace.jsonl`.

Integers are consumed in full by `std::from_chars`, and durations are checked against the target
representation range before conversion. Flags are read as "flag immediately followed by value"
pairs; unknown flags, missing values, non-integers, and out-of-range values all return
`InvalidResource`. As a result, the later composition only deals with typed values and never
re-parses strings.

### Offline Loading and Name Resolution

`runProduct` first calls `engine::loadRuntimeProject` in
`modules/engine/source/engine/runtime-loader.hpp`. That loader reads
`generated/annotations.runtime.toml`, applies a 16 MiB size limit before reading, then reads
`assets/templates/<hash>.png` according to the manifest references, with
`annotation::RecognitionRuntime::create` validating the hash closure and decoding the templates.

Only after loading succeeds does the CLI query `loaded.m_runtime.manifest().catalog()` through
`resolvePageName` and `resolveActionName`. A page name is matched only within `catalog.pages()`;
an action name is matched only within `catalog.recognizers()` against
`annotation::AnnotationType::ActionTarget`, and a page anchor with the same name cannot be treated
as an action. Matching is an exact, case-sensitive string comparison.

`modules/annotation/source/annotation/catalog.cpp`, in `RecognitionCatalog::create`, already
rejects duplicate page names, duplicate recognizer names, and IDs and names that conflict across
resources, so the linear scan never faces the ambiguity of "take the first with the same name".
The failure message lists every available page or action target in catalog order, giving the
operator a diagnostic they can act on directly.

This offline work deliberately happens before touching the desktop: a bad manifest, a missing
template, or an unknown name will not first declare DPI, register a console handler, enumerate
windows, or create capture resources.

### Windows Composition Sequence

After the offline prerequisites succeed, `entry/cli/run-windows.cpp` assembles in strictly the
following order:

1. `ensurePerMonitorAwareV2` in `modules/controller/source/controller/dpi.hpp` declares
   per-monitor-aware V2. Only then are the subsequent client size, client origin, and DPI
   fingerprint under a consistent coordinate interpretation.
2. `platform::ConsoleCancellation::install` in
   `entry/cli/platform/windows-console-cancellation.hpp` registers the Ctrl-C/Ctrl-Break handler
   and obtains a `std::stop_token` for the engine to use.
3. `enumerateCandidates` enumerates windows; the internal `selectCandidate` requires exactly one
   candidate whose title contains `RunArgs::m_selector`. Zero or more than one both return
   `TargetUnavailable`, without guessing based on enumeration order.
4. The selected handle constructs a `TargetSelector`, from which `resolveTarget` yields a
   `ResolvedTarget`, reading `ClientSize`, `WindowHandle`, and the current `TargetGeneration`. The
   current one-shot process uses a fixed `SessionId{1}`.
5. A live `annotation::ProjectFingerprint` is created from the resolved client width/height and the
   candidate window's DPI. It does not replace the manifest fingerprint; the two must be equal in
   recognition and action authorization.
6. `platform::clientOriginDesktop` uses `ClientToScreen` to find the desktop-space origin of client
   `(0, 0)`; together with the client extent it creates a `ClientGeometry`, and then a
   `WgcCaptureSession`.
7. The same handle, session, generation, and client extent create a `DeliveryTarget`, ensuring that
   the capture identity and the delivery identity come from the same target resolution.
8. Create `FileTraceSink`, `platform::WgcFrameSource`, and `platform::ControllerActionSink`, fill
   the CLI arguments into `engine::EngineSessionConfig`, and finally call
   `engine::EngineSession::create`.

`EngineSessionConfig` carries the live fingerprint, the pixel budget, the per-recognition deadline,
the maximum frame age, and the cancellation token. The three adapters are handed to the session as
`std::unique_ptr<engine::FrameSource>`, `std::unique_ptr<engine::ActionSink>`, and
`std::unique_ptr<engine::TraceSink>`; from this point on the session owns the port lifetimes.

The execution stage calls `EngineSession::waitForPage(pageId, timeout, pollInterval)` in
`modules/engine/source/engine/session.hpp`. The returned `PageWait` pairs the matched
`ResolvedPage` with the same `Observation` that produced it, and the CLI then calls
`findAction(actionId)` on that observation, without grabbing another frame for the action.

An absent action is a normal Tier-A outcome: `findAction` returns a successful empty
`std::optional<ActionFound>`, and the CLI produces `RunReport{m_actionDelivered = false}`. When the
action exists, `EngineSession::act` consumes the observation, performs authorization, the
frame-to-client transform, and delivery, and returns an `ActReceipt`; the CLI writes the actual
client click point into the success report.

### The Three Port Adapters

`platform::WgcFrameSource` in `entry/cli/platform/wgc-frame-source.hpp` owns a move-only
`WgcCaptureSession` by value. `capture()` forwards to the session as-is, and
`validateTargetInstance()` also forwards as-is. This thin layer does not duplicate the capture
rules: frame ID, target generation, content-size-change invalidation, and capture stall continue
to be decided by the implementation in `modules/controller/source/controller/capture.hpp`.

`platform::ControllerActionSink` in `entry/cli/platform/controller-action-sink.hpp` owns a
`DeliveryTarget` by value and holds `HeldInputs` and `AuditLog` for that target. Its
`click(point, lease)` passes the `ObservationLease` unchanged to `uf::click` in
`modules/controller/source/controller/input.hpp`. The controller therefore rechecks session,
generation, expiry, coordinate finiteness, client bounds, and the Win32 signed-16-bit encoding
range at the moment of actual delivery.

If any step in the pointer down/up chain fails, the adapter preserves the original error and calls
`releaseHeld` to drain any held input that may remain. A failure of the compensating release only
appends context and never masks the original click failure. Both `AuditLog` and the held state are
owning members of the adapter and never borrow a temporary on the `runProduct` stack.

`FileTraceSink` in `entry/cli/file-trace-sink.hpp` owns a `std::ofstream`. `create` opens the path
in binary + trunc mode and returns a `std::unique_ptr<engine::TraceSink>`; a failure to open is an
`IoFailure`. Each `emit` uses `engine::serializeTraceEvent` to write one JSONL record, appends a
newline, and immediately `flush`es; a write or flush failure likewise returns `IoFailure` to the
engine. This avoids leaving events across emits in the C++ stream buffer, but the code does not
claim a filesystem-level durable-sync guarantee.

### Exit-Code Contract

The strongly typed `ExitCode` in `entry/cli/run.hpp` defines the exit-code contract in one place,
and `run.cpp` maps structured errors to that enum. Only `main.cpp` converts it to `int` with
`std::to_underlying` at the process boundary:

| Exit Code | Meaning |
|---:|---|
| `0` | The help path of the empty command, or an action successfully delivered |
| `1` | Unknown subcommand, argument/resource error, unsupported host, the vast majority of run failures, or an uncaught exception |
| `2` | `TargetCompatibilityUnverified` |
| `3` | The page was resolved, but the specified action target is absent from that observation |
| `4` | `Timeout` |
| `5` | `Cancelled`, or a console stop was already requested when the run failure returned |

Every CLI path returns `ExitCode`, avoiding a mix of `EXIT_FAILURE` and bare integers for the same
contract. `exitCodeForError(error, stopRequested)` checks `stopRequested` first and
`AutomationErrorKind` second. Therefore, when Ctrl-C occurs during a blocking step such as capture,
even if the underlying layer eventually surfaces `CaptureStalled`, `IoFailure`, or `Timeout`, the
operator's cancellation intent is still reported preferentially as `5`. Argument parsing precedes
handler installation, so a parse error is mapped explicitly with `stopRequested=false`; the
non-Windows implementation also always reports that no cancellation was received.

The error text is composed by `formatRunError` from the automation kind, the message, all context,
and, when present, the native error category/value. This is the CLI's single-line diagnostic
format and does not change the classification of the underlying `Error`.

## Constraints That Must Remain True

**Fail-closed.** Resource loading and name resolution complete before any desktop side effect; the
window substring must be unique; a failure at any step of DPI declaration, geometry creation, WGC,
delivery, or trace terminates immediately through `UF_TRY`. When the live fingerprint does not
match the manifest, `RecognitionRuntime` refuses recognition and `authorizeCoordinateAction` again
refuses the action. After action authorization, `EngineSession::act` further revalidates the bound
instance through `FrameSource::validateTargetInstance` before calling the sink, and delivers
nothing on failure.

**Two-layer stale-observation fence.** The engine layer calls `authorizeCoordinateAction` with the
`ObservationLease`, `ResolvedPage`, and `ActionDetection` carried by the observation; the CLI
adapter then forwards the same lease to the controller so that it revalidates session, generation,
and age at post time. A failure at either layer sends no click. After a successful click, the
observation is marked invalid before the potentially failing post-click trace, so that a trace
failure cannot induce a duplicate delivery.

**Determinism.** Argument conversion does not depend on locale; name resolution uses the catalog's
stable order and unique names; window selection rejects multiple matches rather than taking "the
first"; the same `Observation` carries both page and action evidence; the default click point and
coordinate transform are computed by annotation/engine. Trace uses a fixed-schema serializer. Real
window content and arrival timing are not themselves deterministic inputs, but the entry layer
introduces no additional implicit selection.

**Ownership and lifetime are visible.** `RunArgs`, `LoadedRuntime`, the three adapters, and their
system resources all move by value or by `unique_ptr`. `ConsoleCancellation` manages the handler
registration with RAII; the actual `stop_source` is a module-static process-lifetime object, so the
exit-code boundary can still read the stop state after the handler is unregistered. That source,
once stopped, is never reset, which is consistent with the "exactly one run per process" contract.

`engine::Observation` internally holds a non-owning back-reference to its `EngineSession` and
therefore must be shorter-lived than the session; the local scope of `runProduct` satisfies this.
After an observation is moved, the source object is invalidated, and `act` consumes it by rvalue,
so the type and a runtime flag together restrict reuse.

**Strict-background.** The CLI never calls focus, activation, or global-input APIs.
`ControllerActionSink` ultimately enters the `PostMessageW` path in
`modules/controller/source/controller/platform/windows-input.cpp`, delivering integer mouse
messages only to the single resolved HWND, and rejects null and `HWND_BROADCAST`. When the target
becomes inactive, message delivery fails, or compatibility cannot be confirmed, it fails; there is
no fallback that switches to foreground input "in order to succeed".

**Trace is part of the correctness path.** `engine::TraceSink::emit` returns a `Status`, and the
`SessionStarted` of session creation, recognition failures, and authorization- and
delivery-related events can all fail the operation. `FileTraceSink` likewise does not swallow write
errors. This makes "unable to leave the required evidence" an explicit product failure rather than
an invisible best-effort log loss.

## Dependencies

The inbound edge is shell/operator → CLI. What crosses the boundary is string arguments, the
project path, the target-title substring, names, and exit codes; at this layer the CLI is
responsible for readable diagnostics and defaults.

The outbound edge toward `engine` is established through `${PROJECT_NAME}_cli_support` in
`entry/CMakeLists.txt`. What crosses the boundary is:

- `LoadedRuntime` and `EngineSessionConfig`;
- the three owning port implementations;
- `PageId`, `RecognizerId`, `RunReport`, and structured `Error`.

`engine` never sees the HWND, the console handler, file-selection syntax, or the title substring.
In the reverse direction, the CLI never interprets recognizer evidence, page outcomes, or
authorization rules; it only drives the public surface of `waitForPage`, `findAction`, and `act`.

The outbound edge toward `controller` exists only in the Windows build. What crosses the boundary
is the resolved `WindowHandle`, `ClientSize`, `Dpi`, `TargetGeneration`, `ClientGeometry`,
`WgcCaptureSession`, `DeliveryTarget`, and `ObservationLease`. The `Frame` produced by WGC carries
session/generation/frame identity and a coordinate transform, and the final click carries the same
identity lineage back to the controller.

Collaboration with `annotation` happens mainly indirectly through `engine`. The CLI touches
`RecognitionCatalog` directly only for name resolution and creates `ProjectFingerprint` directly
only to express the real target's size/DPI. It does not modify the catalog, nor does it create
authoring resources.

The split of `${PROJECT_NAME}_cli_support` is part of the test architecture, not merely a build
convenience. `entry/cli/main.cpp` keeps only a thin process shell; argument parsing, name
resolution, file trace, error formatting, and exit-code mapping go into a static library that both
the executable and `test-cli` link. On Windows the library additionally adds the real adapters and
links the controller; on other platforms it adds `run-unsupported.cpp`. As a result, the
platform-independent contract can be tested in CI without a Windows desktop, while the product
executable does not need to export internal functions.

## Tests

`tests/cli/test-args.cpp` pins down the complete flag happy path, all optional defaults, the four
required items, unknown/missing/non-integer inputs, the 1/60000 ms boundary of poll, and the
exit-code mapping. Its cancellation-priority case combines `CaptureStalled`, `Timeout`, and
`IoFailure` with `stopRequested=true` to directly prevent underlying errors from overriding the
Ctrl-C intent.

`tests/cli/test-name-resolution.cpp` uses a real `RecognitionCatalog` to pin down page and action
name-to-ID and confirms that a page anchor cannot resolve to an action; the available list for an
unknown name is also part of the test contract.

`tests/cli/test-file-trace-sink.cpp` pins down "one serialized JSONL per emit" and the ordering,
and confirms that a path that cannot be opened returns `IoFailure`. The fixed JSON schema itself is
covered by `tests/engine/test-trace.cpp`.

The safety semantics of the CLI adapters are fixed in layers by downstream tests:

- `tests/engine/test-session.cpp` covers the full observe→resolve→find→act, fingerprint mismatch,
  an unauthorized action, an expired lease, invalidated/cross-session observations, delivery-edge
  target revalidation, cancellation, and `waitForPage` timeout.
- `tests/engine/test-runtime-loader.cpp` covers published-project loading, a bad manifest, a
  missing template, hash mismatch, and the manifest size limit.
- `tests/controller/test-input-revalidation.cpp` covers lease session/generation/age, the
  coordinate range, and a dead HWND; `tests/controller/test-input-held.cpp` covers held-state
  draining and release failure; `tests/controller/test-input.cpp` covers the delivery target and
  held-action identity.
- `tests/controller/test-capture-wgc.cpp` and `tests/controller/test-capture-stall.cpp` cover the
  frame ID, geometry invalidation, options, and stale-arrival behavior forwarded by the WGC
  adapter.
- `tests/controller/test-target.cpp`, `tests/controller/test-discovery.cpp`, and
  `tests/controller/test-dpi.cpp` fix target resolution, generation, Win32 discovery errors, and
  DPI fail-closed classification.

The `test-cli` in `tests/CMakeLists.txt` compiles in only the three platform-independent CLI test
files directly and links `${PROJECT_NAME}_cli_support`. There is currently no direct unit test
covering `main`/`dispatch`, `selectCandidate`, console registration, the client-origin adapter, or
the entire real Windows composition; these belong to the on-hardware smoke/E2E verification surface
and cannot be substituted by the green of the existing offline tests.

## Future Extensions

`docs/plans/2026-07-23-engine-architecture.md` is the direct authority for the current engine/CLI
composition. It explicitly positions the CLI as the Windows composition root of Phase 3, positions
the fixed C++ flow as a smoke flow, and leaves the task language to Luau. Therefore new task
orchestration should attach to the observe/act/wait surface of `EngineSession` or to a script
binding, and should not continue to pile a declarative language into `runProduct`.

The same plan lists the following seams:

- The P3 second platform is integrated by implementing the same `FrameSource`, `ActionSink`, and
  `TraceSink`; the CLI host implementation can replace `run-unsupported.cpp`, and the engine need
  not be aware of the platform.
- B2 Luau provides a 1:1 binding for `Observation` and observe/find/act/wait; the shape of the
  existing C++ API is retained precisely to avoid refactoring at that time.
- D6/P1 popup handling attaches to the existing no-op hook `EngineSession::sweepKnownPopups`,
  rather than being stuffed into window discovery or CLI argument parsing.
- P0-C: if on-hardware UIPI verification requires a separate elevated process, the plan requires
  copying the protocol semantics of the m0-demo input-agent into the runner adapter layer, rather
  than linking the already-frozen m0-demo.

`docs/plans/2026-07-21-product-form-and-roadmap.md` defines the current form as "P0 CLI
single-task, strict-background, reliable cancellation" and defines P2 as a tray-resident App. At
that point the composition boundary of `EngineSession` and the three ports can be reused, but the
process lifecycle, task list, scheduled tasks, and UI state belong to a new app shell and should
not become CLI or engine policy in reverse.

B3 of that roadmap further requires hard cancellation and long-run stability;
`docs/plans/2026-07-20-post-port-win32-robustness.md` records still-open controller issues such as
capture-wait cancellation, stall timeout, and the pairing of lease age. The related improvements
should land in the WGC/controller capability and its adapter connection points, and should
preserve the current principle: cancellation or capture uncertainty may only terminate the action,
and may not relax the lease or switch to foreground input.

Resolution adaptation, OCR, pause/resume, and resident scheduling are also explicitly placed
outside the current Phase 3 by the plans above. When extending them, one should add new
recognition/engine/app capabilities along the existing boundaries; in particular, one must not
bypass the current size/DPI fail-closed contract by forging a live fingerprint in the CLI.
