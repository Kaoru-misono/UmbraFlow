# entry/cli Architecture Knowledge

`entry/cli` is the command-line entry point for `umbra-flow` and the Windows composition root for
one session's desktop binding. It connects the platform-independent `engine` ports to the real
capture and background-input capabilities of `controller`, and owns arguments and process exit
codes.

**`umbra-flow` has two subcommands, `run` and `drive`, and they are two front-ends over one
capability surface.** `run` loads a project-owned Luau task and lets the trusted framework drive it;
`drive` takes JSON-line commands from an operator outside the process and drives the same
primitives. Neither can reach anything the other cannot, and a single generation admits exactly one
of them: `TaskHost::Generation::claimFrontEnd` latches the first front-end to reach a generation --
whichever of `startTask` and `startOperatorSession` arrives first -- and refuses the other for that
generation's life, reported as `UnsupportedCapability` rather than as an invariant failure, because
asking is legitimate and nothing in the binary is broken when it happens (2026-07-30, `ed38124`).

The run lifecycle itself is no longer here. Since 2026-07-29 it lives in `task::TaskHost`
(`modules/task/source/task/task-host.hpp`), per `docs/plans/2026-07-29-three-layer-task-system.md`
section 13. What remains in this directory is: parse the arguments, build the adapters and the ports
they implement, call the host, and print the report it returns — plus, since 2026-07-30, the
operator protocol itself, which is a text encoding of that same host surface and holds no policy.

## Entry-Point Responsibilities

This directory owns three kinds of product boundary.

The first is the process boundary. `entry/cli/main.cpp` copies `argv` into an owning
`std::vector<std::string>`, recognizes the `run` and `drive` subcommands, emits usage, the one-line
report of a session that started, or an error, and collapses every outcome into an integer exit
code. A started run always names its task, source hash, seed, and trace path, whether it completed,
was cancelled, or failed; a started drive session names its queue, results, and trace paths instead,
because it ran no task and therefore has no task name or source hash to report. A failure adds the
rendered error line. With no subcommand it prints the project name and **both** usages — the two
modes are equal citizens, so neither is the one a reader is shown by default.

The second is the command boundary. `RunArgs` and `DriveArgs` in `entry/cli/args.hpp` are the
complete value objects for one `run` and one `drive`, parsed by `parseRunArguments` and
`parseDriveArguments` in `entry/cli/args.cpp`, which turn paired value flags into paths, names,
budgets, and monotonic-clock durations. After a successful parse there are no "unset" optional
fields: the required items must be non-empty, and the remaining fields already carry safe defaults.

**The two argument shapes state the mutual exclusion before anything else does.** There is no
`--task` on `DriveArgs` and no `--queue` on `RunArgs`, so a session that has both a task and a
command queue cannot be spelled. `main.cpp` chooses the mode from one subcommand token and neither
handler can reach the other. Neither of those is the structural refusal, though — that is
`TaskHost`'s per-generation front-end claim, which refuses the second front-end however it was
reached.

The third is the composition boundary. The Windows implementations, `runProduct` in
`entry/cli/run-windows.cpp` and `driveProduct` in `entry/cli/drive-windows.cpp`, select a real
window and create the two adapters for capture and input, which they hand to `task::TaskHost`
inside a `task::TaskRunConfig`. The CLI never names `engine::EngineSession`; the host builds and
owns it. The desktop binding both commands need -- DPI awareness, window enumeration, candidate
selection, capture and controller setup -- lives once in `entry/cli/platform/target-binding.{hpp,cpp}`
and returns a `BoundTarget`, so the two commands cannot end up with separate answers to "which
window did you mean". Non-Windows builds use `entry/cli/run-unsupported.cpp` and
`entry/cli/drive-unsupported.cpp`, which keep the same product binary and testable command layer
but have both product entry points explicitly return `UnsupportedCapability`.

This directory deliberately does not own the following responsibilities:

- It does not own recognition, page determination, action authorization, or the observation
  lifecycle; these belong to `modules/annotation/` and `modules/engine/`.
- It does not own Win32 window enumeration, WGC, target generation, lease validation, or message
  delivery; these belong to `modules/controller/`, and the CLI only does thin adaptation and
  assembly.
- It does not define a task language or a general workflow, and it no longer runs a fixed one. The
  `runSmokeFlow` single-step path -- wait for one page, look for one action target, click once --
  was deleted on 2026-07-29 (`docs/plans/2026-07-29-three-layer-task-system.md` section 16).
  `runProduct` binds a target and calls `TaskHost::startTask`; what happens inside the run is the
  project's task script.
- It does not translate resource names. A script names its resources directly as
  `uf.pages.NAME` and `uf.recognizers.NAME` (the root was renamed from `umbra` to `uf` on
  2026-07-29, `2f4af93`), and `task::validateScriptResources` in
  `modules/task/source/task/script-validator.hpp` closes every such reference against the
  capability surface before a VM exists. `entry/cli/name-resolution.{hpp,cpp}` existed only to
  translate the two deleted flags and was deleted with them.
- It does not read authoring documents, nor does it generate manifests or templates, and it does
  not load them either: it names a published project directory, and `TaskHost::loadProject` reads
  it through `engine::loadRuntimeProject`.
- It does not provide a foreground or global-input fallback. A background-delivery failure is a
  product failure.
- It does not take on a daemon, tray, scheduler, or multi-task lifecycle; the current design is
  one session per process — one `run` or one `drive`, never both.
- It does not add capability in `drive`. The operator protocol is a text encoding of the private
  capability surface plus two loops built out of it; every refusal an operator meets is the refusal
  a task meets, because both call the same `TaskContext`.

This boundary explains why target discovery stays in the entry layer rather than moving into
`engine`: `engine::IFrameSource` receives "one already-bound target" and thereby stays
platform-independent, while window-title selection, DPI declaration, and Win32 geometry are the
product's policy for how it obtains that port on Windows.

## Command Execution Flow

### Command-Line Entry Point

The `dispatch` in `entry/cli/main.cpp` has three public product paths:

- Empty arguments: print generated `application::k_name` and `usageText()` — both subcommands'
  usage — and return `0`.
- First argument is `run`: hand the remaining arguments to `dispatchRun`.
- First argument is `drive`: hand the remaining arguments to `dispatchDrive`.

Any other first argument is treated as an unknown subcommand, prints an error and both usages, and
returns `1`. `main` also checks that `argumentCount` can be safely converted and is non-zero, and
it catches `std::exception` and unknown exceptions at the outermost level; exceptions never cross
the process boundary.

The `RunArgs` in `entry/cli/args.hpp` corresponds to the following real CLI surface:

- `--project DIR`, `--selector TITLE-SUBSTRING`, and `--task NAME` are required **for `run`**.
  `--task` names `tasks/NAME.luau` inside the published project.
- `--budget N` defaults to `k_defaultPixelComparisonBudget == 1 << 31` and bounds the number of
  pixel comparisons in a single recognition. It was `1 << 28` until 2026-07-30 (`2429578`), which
  could not finish an ordinary page: `evaluateGrayPage` shares one budget across **every anchor of a
  page**, so the figure to cover is the sum rather than one search. The envelope stated at the
  constant is a 200x50 template in a 480x90 search region — 11,521 positions at 10,000 pixels — and
  sixteen such anchors, 1,843,360,000. A whole-frame search for a small template is deliberately
  still outside the default, at about 1.9x the ceiling, so such a project must author a search
  region or raise `--budget` on purpose. The same constant is shared with `umbra-authoring`, so one
  number governs verification and the run.
- `--recognition-timeout MS` defaults to 2000 ms and is the deadline for each recognition.
- `--max-frame-age MS` defaults to 750 ms and determines how long an observation lease remains
  usable for an action.
- `--trace PATH` defaults to `umbra-flow-trace.jsonl`.

`DriveArgs` binds a target and a project exactly as `RunArgs` does, and `--budget`,
`--recognition-timeout`, `--max-frame-age` and `--trace` are the same fields with the same
defaults, because the two front-ends must not be able to run under different guarantees. What
replaces `--task` is the pair of IPC files:

- `--project DIR`, `--selector TITLE-SUBSTRING`, `--queue PATH`, and `--results PATH` are required
  **for `drive`**.
- `--idle-timeout S` defaults to 120 s and ends a session whose command queue has gone quiet, so an
  operator who walks away, or a driving process that died, does not leave a session holding a
  capture device and a bound target indefinitely. The figure matches the m0-demo input agent's own
  idle timeout, which is the protocol this one follows.

There are deliberately **no timeout, poll-interval or retry defaults on `DriveArgs`**. Those are
policy; every convenience command requires them as fields (see below), and a CLI flag for them
would be a second place task-side policy could live.

Integers are consumed in full by `std::from_chars`, and durations are checked against the target
representation range before conversion. Flags are read as "flag immediately followed by value"
pairs; unknown flags, missing values, non-integers, and out-of-range values all return
`InvalidResource`. As a result, the later composition only deals with typed values and never
re-parses strings.

**The two page-wait flags are gone.** `--timeout SEC` and `--poll MS` used to be the host-side
defaults for a script's page wait, forwarded through `task::TaskRunConfig` into `TaskContextConfig`.
Once the wait loop moved wholesale into the trusted Luau framework on 2026-07-29 (`d1a0685`), no
host code read those values any more, so they went along with both config fields; today the
unknown-flag rule above **refuses them as unknown arguments**. How long to wait and how often to
re-observe now live in `modules/task/runtime/ctx.luau` as `k_defaultTimeoutMillis` (600000 ms) and
`k_defaultPollMillis` (500 ms), which a script overrides through `{ timeout_ms, poll_ms }`. The
surviving `--recognition-timeout` is the deadline for one recognition and has nothing to do with a
page wait.

### The Operator Protocol (`drive`)

`entry/cli/drive-protocol.{hpp,cpp}` defines the wire protocol and `entry/cli/drive.{hpp,cpp}` runs
the loop. Commands arrive as **JSON lines appended to `--queue`**, and **one JSON result line per
command is appended to `--results` and flushed immediately**, so an operator that reads the file
sees a command's answer before the next command is executed. `k_maxDriveCommandBytes` caps one line
at 64 KiB, matching the m0-demo input agent's ceiling; a command is a handful of scalars, so
anything near it is a malformed line rather than a large one.

**Three refusals on the IPC paths, checked by `validateDriveIpcPaths` before the desktop is touched
at all.** The queue must already exist, because a session that created it would race the operator
appending to it. The two paths must be distinct, because a session reading its own results would
re-execute them. And **the results path must not already exist**, so a stale results file from an
earlier session can never be mistaken for this one's and nothing is silently appended to or
clobbered. That last guard is the m0-demo input agent's, carried over deliberately — it caught two
real operator mistakes.

**The queue is read by byte offset.** `QueueReader` keeps the session's own offset into the file, so
a line is executed exactly once however many times the queue is polled, and **a partial trailing
line is held back until its terminator arrives** — an operator appending a command in two writes
never has half of it executed. A queue that shrank was replaced or truncated under the session,
which makes the offset name a different file's bytes, and is refused as `InvalidResource`.

**Two command layers, one copy of the policy.** Layer one is the private capability surface
verbatim, one command per primitive with the same name, arguments and failure modes the Luau
surface has, with the opaque handles replaced by integer ids because an operator has only text:
`cycle_open`, `cycle_close`, `cycle_page`, `cycle_find`, `cycle_click`, `key`, `settle`, `deadline`,
`wait`, plus `quit`. Each maps to exactly one `task::OperatorSession` verb, which maps to exactly
one `TaskContext` call — the same call `ctx.luau` makes. Layer two is convenience: `wait_page` and
`find_click`, each replacing a hand-written loop.

The constraint that makes layer two safe is that **a convenience command carries no policy defaults
of its own**. Every timeout and poll interval is a **required** field (`timeout_ms`, `poll_ms`),
rejected rather than filled in, so `modules/task/runtime/ctx.luau` keeps its defaults and stays the
only place task-side policy lives; there is no second copy in C++ to drift out of step with it. A
field a command does not accept is likewise refused rather than ignored. Layer two composes
layer-one verbs and nothing else: it decides when to stop looping, and the caller decides every
number that decision uses.

A `DriveResult` has one line shape rather than twelve — `ok` plus the optional values a verb can
produce — so an operator reads results with one parser. `ok` is the distinction every caller
depends on, so a refused command never reports it true. A failure line carries the domain's own
wire spelling of the kind in `errorKind`, the same string the trace line and a task's Tier B error
carry. A refused command is normally an ordinary outcome — the operator reads the line and tries
something else — with one exception: a `Cancelled` refusal ends the session, because once the
generation is spent every later primitive refuses on the terminal latch and continuing would only
spin the queue until the idle timeout.

`task::OperatorSession` (`modules/task/source/task/operator-session.hpp`) is the consumer on the
other side, and it is a **sibling** of the trusted Luau framework rather than a route into it. The
guarantee layer sits below Luau — the cycle ledger, the four-requisite click authorization, the
fingerprint check and the trace are all C++ behind `TaskContext` — so an operator gets exactly the
primitives and exactly the refusals a task gets. There is no chunk, no source and no string that
becomes code, and the sandbox's closed eval routes stay closed. An operator may name only what a
task may name: the session holds a copy of the same `CapabilitySurface` the `uf.recognizers` and
`uf.pages` tables are built from. `raise`, `emit` and `random` are deliberately absent — an operator
has no framework structure of its own to record, and admitting `emit` would put a second author on
the `framework.*` events the trace stream validator now refuses outright on an operator stream.

### Project Loading

`runProduct` calls `task::TaskHost::loadProject` with the project directory and a
`task::TaskHostConfig` carrying the console stop token. The host calls `engine::loadRuntimeProject`
in `modules/engine/source/engine/runtime-loader.hpp`, which reads
`generated/annotations.runtime.toml`, applies a 16 MiB size limit before reading, then reads
`assets/templates/<hash>.png` according to the manifest references, with
`annotation::RecognitionRuntime::create` validating the hash closure and decoding the templates.

The host then builds the script-visible capability surface from
`loaded.runtime.manifest().catalog()` with `task::CapabilitySurface::create`, registers the loaded
runtime and that surface together as one generation, and returns the generation's `GenerationId`.
A generation outlives every run made against it: the runtime and the surface are per project, while
the trace recorder, the engine session, the task context, and the VM are per run.

This work deliberately happens before touching the desktop: a bad project path, a corrupt manifest,
or a missing template fails without declaring DPI, enumerating windows, or creating capture
resources. That ordering is load-bearing and was re-verified live on 2026-07-29 -- binding the
target first made a bad `--project` report a window error instead of the manifest error.

### Windows Composition Sequence

`entry/cli/run-windows.cpp` assembles in strictly the following order. Steps 3 through 8 are no
longer written there: since 2026-07-30 (`ed38124`) they live in `platform::bindTarget` in
`entry/cli/platform/target-binding.cpp` and are shared verbatim with `drive-windows.cpp`, which
performs steps 1, 2 and 3-8 in the same order and differs only in calling
`TaskHost::startOperatorSession` where `run` calls `startTask`. `drive` additionally validates its
IPC paths before step 1, so a results file that already exists fails before a console handler is
even installed.

1. `platform::ConsoleCancellation::install` in
   `entry/cli/platform/windows-console-cancellation.hpp` registers the Ctrl-C/Ctrl-Break handler
   and obtains a `std::stop_token`. It is installed first, before anything can block, and its
   registration is held ahead of every later local so it is removed last. The token becomes
   `TaskHostConfig::externalCancellation`, which the generation composes with the stop source
   `TaskHost::cancel` drives, so an external stop and an explicit cancel feed one source.
2. `TaskHost::loadProject` loads the project, as described above, before any desktop contact.
3. `ensurePerMonitorAwareV2` in `modules/controller/source/controller/dpi.hpp` declares
   per-monitor-aware V2. Only then are the subsequent client size, client origin, and DPI
   fingerprint under a consistent coordinate interpretation.
4. `enumerateCandidates` enumerates windows; `selectCandidate` in
   `entry/cli/candidate-selection.hpp` requires exactly one capturable candidate -- visible and not
   minimized -- whose title contains `RunArgs::selector`. Zero or more than one both return
   `TargetUnavailable`, without guessing based on enumeration order.
5. The selected handle constructs a `TargetSelector`, from which `resolveTarget` yields a
   `ResolvedTarget`, reading `ClientSize`, `WindowHandle`, and the current `TargetGeneration`. The
   current one-shot process uses a fixed `CaptureSessionId{1}`.
6. A live `annotation::ProjectFingerprint` is created from the resolved client width/height and the
   candidate window's DPI. It does not replace the manifest fingerprint; the two must be equal in
   recognition and action authorization.
7. `platform::clientOriginDesktop` uses `ClientToScreen` to find the desktop-space origin of client
   `(0, 0)`; together with the client extent it creates a `ClientGeometry`, and then a
   `WgcCaptureSession`. The same handle, session, generation, and client extent create a
   `DeliveryTarget`, ensuring that the capture identity and the delivery identity come from the
   same target resolution.
8. `platform::WgcFrameSource` and `platform::ControllerActionSink` wrap those two resources, and
   together with the live fingerprint and the CLI's tuning values they fill a `task::TaskRunConfig`
   that is moved into `TaskHost::startTask`.

`TaskRunConfig` is a move-in ownership boundary rather than an ordinary config: it carries
`std::unique_ptr<engine::IFrameSource>` and `std::unique_ptr<engine::IActionSink>`, so the caller's
copy is left empty and the run owns the port lifetimes. Alongside them it carries the live
fingerprint, the pixel budget, the per-recognition deadline, the maximum frame age, and the trace
path. **It carries no page-wait budget**: how long a task waits and how often it re-observes are
decided by the task in Luau, and a host-side fallback the framework cannot read would be a value
nothing reads.

`startTask` is where the run happens. It loads and validates the task, opens the trace, and builds
the `trace::TraceRecorder`, the `engine::EngineSession`, the `task::TaskContext`, and the VM for
exactly the run's duration, in that order; the recorder is declared before every borrower and held
through a `unique_ptr`, so its address is fixed and all three die before it on every path,
including early returns. The CLI observes none of this: it receives a `task::TaskRunReport` and
prints one line.

### The Two Port Adapters

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

Its second verb, `pressKey(key, actionGeneration)`, forwards to `uf::keyPress` with
`KeyInput::fromKeyName(key)`, and passes a `TargetGeneration` where `click` passes a lease. That is
not an oversight: a keystroke names no coordinate, so there is no rect whose position could have
gone stale and nothing for a frame age to protect; what must still hold is that the keystroke
reaches the target instance the observation came from, which is exactly what the generation carries.
See `module-engine.md` for the full contrast between the two authorizations.

If any step in the pointer or key down/up chain fails, the adapter preserves the original error and
calls `releaseHeld` to drain any held input that may remain — a press that landed while its release
did not would otherwise strand the key down in the target. A failure of the compensating release
only appends context and never masks the original failure. Both `AuditLog` and the held state are
owning members of the adapter and never borrow a temporary on the `runProduct` stack.

The third port is no longer a CLI adapter. `trace::FileTraceSink` lives in
`modules/trace/source/trace/file-sink.hpp`, and `TaskHost::startTask` opens it -- after the task has
loaded and validated, so a misspelled task name leaves no evidence file behind. `create` opens the
path in binary + trunc mode and returns a `std::unique_ptr<trace::ITraceSink>`; a failure to open is
an `IoFailure`. Each `emit` writes one serialized JSONL record, appends a newline, and immediately
`flush`es; a write or flush failure likewise returns `IoFailure` to its caller. This avoids leaving
events across emits in the C++ stream buffer, but the code does not claim a filesystem-level
durable-sync guarantee.

### Exit-Code Contract

The strongly typed `ExitCode` in `entry/cli/run.hpp` defines the exit-code contract in one place,
and `run.cpp` maps structured errors to that enum. Only `main.cpp` converts it to `int` with
`std::to_underlying` at the process boundary:

| Exit Code | Meaning |
|---:|---|
| `0` | The help path of the empty command, or a task or drive session that ran to completion |
| `1` | Unknown subcommand, argument/resource error, unsupported host, the vast majority of run failures, or an uncaught exception |
| `2` | `TargetCompatibilityUnverified` |
| `4` | `Timeout` |
| `5` | `Cancelled`, or a console stop was already requested when the run failure returned |

`3` is deliberately absent from that table and stays absent. It was `ActionAbsent`, and its only
producer was the smoke path removed on 2026-07-29; a task script now decides for itself what an
absent target means. The value is left unused rather than reassigned, because an operator or script
reading an old `3` in a log must never be told it meant something else.

Every CLI path returns `ExitCode`, avoiding a mix of `EXIT_FAILURE` and bare integers for the same
contract. `exitCodeForError(error, stopRequested)` checks `stopRequested` first and
`AutomationErrorKind` second. Therefore, when Ctrl-C occurs during a blocking step such as capture,
even if the underlying layer eventually surfaces `CaptureStalled`, `IoFailure`, or `Timeout`, the
operator's cancellation intent is still reported preferentially as `5`. Argument parsing precedes
handler installation, so a parse error is mapped explicitly with `stopRequested=false`; the
non-Windows implementation also always reports that no cancellation was received.

A session that started is mapped by `exitCodeForReport(report, stopRequested)`, the single
definition of that mapping, used by `run` and `drive` alike because both end in a
`task::TaskRunReport`: a report carrying no failure is `Success`, and every other report defers to
`exitCodeForError` over the failure that ended it. A cancelled run therefore reports `5` because its
failure kind is `Cancelled`, not because this function knows about cancellation separately, and a
run that never started reports the same kind the same way.

The error text is composed by `formatRunError` from the automation kind, the message, all context,
and, when present, the native error category/value. This is the CLI's single-line diagnostic
format and does not change the classification of the underlying `Error`.

## Constraints That Must Remain True

**Fail-closed.** Project loading completes before any desktop side effect, and the task's script
resource validation completes before a VM exists and before the trace file is opened, so a
misspelled task or an unresolvable resource name leaves neither a VM nor an evidence file behind;
the window substring must be unique; a failure at any step of DPI declaration, geometry creation, WGC,
delivery, or trace terminates immediately through `UF_TRY`. When the live fingerprint does not
match the manifest, `RecognitionRuntime` refuses recognition and `authorizeCoordinateAction` again
refuses the action. After action authorization, `EngineSession::act` further revalidates the bound
instance through `IFrameSource::validateTargetInstance` before calling the sink, and delivers
nothing on failure.

**Two-layer stale-observation fence.** The engine layer calls `authorizeCoordinateAction` with the
`ObservationLease`, `ResolvedPage`, and `ActionDetection` carried by the observation; the CLI
adapter then forwards the same lease to the controller so that it revalidates session, generation,
and age at post time. A failure at either layer sends no click. After a successful click, the
observation is marked invalid before the potentially failing post-click trace, so that a trace
failure cannot induce a duplicate delivery.

**Determinism.** Argument conversion does not depend on locale; script resource validation resolves
every name against the capability surface's stable, catalog-ordered handle lists; window selection
rejects multiple matches rather than taking "the first"; the same `Observation` carries both page
and action evidence; the default click point and coordinate transform are computed by
annotation/engine. Trace uses a fixed-schema serializer. `TaskHost` draws a fresh seed per run from
`std::random_device` and stamps it into `run.started`, so a run's random sequence replays from its
own record; a silently constant seed would look correct in a trace while destroying that property.
Real window content and arrival timing are not themselves deterministic inputs, but the entry layer
introduces no additional implicit selection.

**Ownership and lifetime are visible.** `RunArgs` and the two adapters handed to `TaskHost` in a
`TaskRunConfig` move by value or by `unique_ptr`; `LoadedRuntime` is owned by the generation and
copied once per run, never per frame, so a generation still has a runtime for its next run.
`ConsoleCancellation` manages the handler
registration with RAII; the actual `stop_source` is a module-static process-lifetime object, so the
exit-code boundary can still read the stop state after the handler is unregistered. That source,
once stopped, is never reset, which is consistent with the "exactly one run per process" contract.

`engine::Observation` does not borrow its `EngineSession`. It shares only a private immutable
identity token with the session that vended it, so moving the session does not leave a dangling
back-reference and a foreign session can still reject the handle. After an observation is moved,
the source object is invalidated, and `act` consumes it by rvalue, so the type and a runtime flag
together restrict reuse.

**Strict-background.** The CLI never calls focus, activation, or global-input APIs.
`ControllerActionSink` ultimately enters the `PostMessageW` path in
`modules/controller/source/controller/platform/windows-input.cpp`, delivering integer mouse
messages -- and, since 2026-07-30, key messages -- only to the single resolved HWND, and rejects
null and `HWND_BROADCAST`. `pressKey` takes the same route with the same audit record and the same
`HWND_BROADCAST` refusal, and holds nothing between down and up. When the target
becomes inactive, message delivery fails, or compatibility cannot be confirmed, it fails; there is
no fallback that switches to foreground input "in order to succeed".

**Trace is part of the correctness path.** `trace::ITraceSink::emit` returns a `Status`, and the run
bracket (`run.started`, `run.resources_validated`, `run.finished`), recognition failures, and
authorization- and delivery-related events can all fail the operation. `trace::FileTraceSink`
likewise does not swallow write errors. This makes "unable to leave the required evidence" an
explicit product failure rather than an invisible best-effort log loss. It holds at the closing end
too: when the run itself succeeded but `run.finished` could not be written, `startTask` puts that
`IoFailure` into the report, so the run is reported Failed. An incomplete trace is not a completed
run. The run's own failure always takes precedence, so this only surfaces when there was no other.

## Dependencies

The inbound edge is shell/operator → CLI. What crosses the boundary is string arguments, the
project path, the target-title substring, names, and exit codes; at this layer the CLI is
responsible for readable diagnostics and defaults.

The outbound edge now runs toward `task`, and is established through `${PROJECT_NAME}_cli_support`
in `entry/CMakeLists.txt`, which links `${PROJECT_NAME}_task` PUBLIC on every host -- the exit-code
boundary maps a `task::TaskRunReport`, so the task module is part of this library's interface even
where the Windows composition does not build. `${PROJECT_NAME}_engine` stays PUBLIC as well,
because the two adapters implement engine ports. What crosses the boundary is:

- the two owning port implementations plus the live `annotation::ProjectFingerprint`, moved in as a
  `task::TaskRunConfig`, together with the pixel budget, the recognition deadline, and the maximum
  frame age;
- the project path, the task name, and the trace path;
- `GenerationId`, `task::TaskRunReport`, and structured `Error`;
- for `drive`, a `std::unique_ptr<task::OperatorSession>` handed back by `startOperatorSession`,
  which the CLI drives verb by verb and then closes with `OperatorSession::finish`. `startTask`
  blocks for the whole run; `startOperatorSession` does not, which is the only lifecycle difference
  between the two front-ends.

`task` and `engine` never see the HWND, the console handler, file-selection syntax, the title
substring, or the queue and results files. In the reverse direction, the CLI never interprets
recognizer evidence, page outcomes, or authorization rules, and no longer drives any `EngineSession`
verb: it calls `loadProject` and then either `startTask` or `startOperatorSession`, and reads the
report.

The outbound edge toward `controller` exists only in the Windows build. What crosses the boundary
is the resolved `WindowHandle`, `ClientSize`, `Dpi`, `TargetGeneration`, `ClientGeometry`,
`WgcCaptureSession`, `DeliveryTarget`, and `ObservationLease`. The `Frame` produced by WGC carries
session/generation/frame identity and a coordinate transform, and the final click carries the same
identity lineage back to the controller.

Collaboration with `annotation` is now fully indirect. The CLI creates a `ProjectFingerprint` to
express the real target's size/DPI and touches nothing else in `annotation`: it does not read the
`RecognitionCatalog`, does not modify it, and does not create authoring resources.

The split of `${PROJECT_NAME}_cli_support` is part of the test architecture, not merely a build
convenience. `entry/cli/main.cpp` keeps only a thin process shell; argument parsing, error
formatting, and exit-code mapping go into a static library that both the executable and `test-cli`
link. On Windows the library additionally adds the real adapters and
links the controller; on other platforms it adds `run-unsupported.cpp`. As a result, the
platform-independent contract can be tested in CI without a Windows desktop, while the product
executable does not need to export internal functions.

As decided on 2026-07-28, the repository-root `manifest.txt` is the canonical source for the
application name and version. Top-level CMake derives `PROJECT_NAME`/`PROJECT_VERSION` from it, and
`entry/CMakeLists.txt` configures `application-info.hpp` into the build tree for the CLI executable.
`main.cpp` therefore prints typed generated metadata without an application constant in `core` or a
global compile-definition macro.

## Tests

`tests/cli/test-args.cpp` pins down the complete flag happy path, all optional defaults, the three
required items, unknown/missing/non-integer inputs, the 1/60000 ms boundary of poll, and both
exit-code mappings -- `exitCodeForError` per failure kind and `exitCodeForReport` per run outcome,
including a completed run that reports success even though a stop arrived while it was finishing. It
also pins that `--page` and `--action` are refused rather than ignored, so a stale invocation fails
loudly instead of quietly running a different task, and that the usage text names `--task` and
neither removed flag. Its cancellation-priority case combines `CaptureStalled`, `Timeout`, and
`IoFailure` with `stopRequested=true` to directly prevent underlying errors from overriding the
Ctrl-C intent.

`tests/cli/test-drive-protocol.cpp` pins the operator protocol on every host, because it names no
Win32 type: every layer-one command's parse, the required-field refusals that keep the convenience
commands free of policy defaults, a field a command does not accept being refused rather than
ignored, the result-line shape, and the three IPC path refusals.

`tests/task/test-operator-front-end.cpp` pins the session below it by running the same scenario down
both paths: a click with no same-frame page and a click on an expired frame are refused identically
on the task and operator paths, an operator names exactly what a task names and nothing else, every
operator trace line is attributed to the operator front-end, and an operator stream refuses a
`framework.*` event outright. It also pins that a delivered key consumes its cycle, that the refusal
the next verb then gets **names no click** — a keystroke spent that cycle (2026-07-30, `2075404`).
`tests/task/test-task-host.cpp` holds the exclusion itself: a generation drives one front-end,
whichever arrives first.

`tests/task/test-task-host.cpp` covers the run lifecycle that used to live here. It publishes a real
annotation project into a temporary directory and drives `TaskHost` end to end against fake ports,
then reads the trace file back: the acceptance case asserts the whole ordered
`umbraflow-trace/v1` bracket from `run.started` through the engine and `task.native_call` events to
`run.finished`, under one sequence and one run and generation identity. It also pins that two runs
of the same task draw different seeds and that the reported seed is the one `run.started` recorded,
that a failed run is reported rather than failing the call, that a missing task fails before any
trace file is opened, and that the P2 verbs report `UnsupportedCapability`.

The safety semantics of the CLI adapters are fixed in layers by downstream tests:

- `tests/engine/test-session.cpp` covers the full observe→resolve→find→act, fingerprint mismatch,
  an unauthorized action, an expired lease, invalidated/cross-session observations, delivery-edge
  target revalidation, cancellation, and whether the `CaptureBudget` `observe` hands out is a real
  deadline.
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

The `test-cli` in `tests/CMakeLists.txt` compiles in `cli/test-args.cpp` and
`cli/test-drive-protocol.cpp` on every host, adds `cli/test-candidate-selection.cpp` only on Windows
where `selectCandidate` is compiled at all, and links `${PROJECT_NAME}_cli_support`. There is
currently no direct unit test covering `main`/`dispatch`, console registration, the client-origin
adapter, `platform::bindTarget`, or the entire real Windows composition; these belong to the
on-hardware smoke/E2E verification surface and cannot be substituted by the green of the existing
offline tests. The run lifecycle is no longer on that list:
it moved into `TaskHost` precisely so that it became reachable from a test.

## Future Extensions

`docs/plans/2026-07-29-three-layer-task-system.md` is the direct authority for the current
CLI/task composition. Its section 13 fixes the `TaskHost` verb set from P0 through P2, so the API
surface does not change when a resident host replaces the CLI, and its section 16 records the
removal of the smoke flow together with `--page`, `--action`, and `ExitCode::ActionAbsent`.
New orchestration therefore belongs in the trusted Luau framework or in `TaskHost`, and must not
come back into `runProduct`.

`docs/plans/2026-07-23-engine-architecture.md` remains the authority for the engine/port
composition itself, and lists the following seams:

- The P3 second platform is integrated by implementing the same `IFrameSource`, `IActionSink`, and
  `ITraceSink`; the CLI host implementation can replace `run-unsupported.cpp`, and the engine need
  not be aware of the platform.
- B2 Luau provides a 1:1 binding for `Observation` and observe/find/act/wait; the shape of the
  existing C++ API is retained precisely to avoid refactoring at that time.
- D6/P1 popup handling **is no longer in the engine**: the `sweepKnownPopups` no-op hook was deleted
  along with `waitForPage` on 2026-07-29 (`8b16f2d`), and popups are now handled by the trusted Luau
  framework's interrupt registry (declared with `task.interrupt`, matched on every turn of
  `ctx:wait_for_page`). The conclusion for the CLI is unchanged: it still does not belong in window
  discovery or argument parsing.
- P0-C: if on-hardware UIPI verification requires a separate elevated process, the plan requires
  copying the protocol semantics of the m0-demo input-agent into the runner adapter layer, rather
  than linking the already-frozen m0-demo.

The `drive` front-end does not change that authority; it is a second consumer of the same
`TaskHost` surface, which is why it needed no new host verb beyond `startOperatorSession`. Anything
an operator would want that a task cannot do belongs on the capability surface, where both would
get it, and not in `drive-protocol.cpp`.

`docs/plans/2026-07-21-product-form-and-roadmap.md` defines the current form as "P0 CLI
single-task, strict-background, reliable cancellation" and defines P2 as a tray-resident App. At
that point the reusable boundary is `TaskHost` plus the ports a host binds for it, but the process
lifecycle, task list, scheduled tasks, and UI state belong to a new app shell and should not become
CLI, task, or engine policy in reverse.

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
