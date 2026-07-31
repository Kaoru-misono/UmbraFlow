# `modules/controller` architecture knowledge

This document describes the current Windows implementation of `modules/controller`. The overall
direction comes from `docs/plans/2026-07-21-product-form-and-roadmap.md`, and the outstanding Win32
hardening is governed by `docs/plans/2026-07-20-post-port-win32-robustness.md`; where the plans and
the code disagree, the code below is authoritative, and requirements that are not yet implemented are
placed under "Future Extensions".

## Module Responsibilities

`modules/controller` is the Windows desktop capability adaptation layer. It converts windows,
processes, WGC, D3D11, and window messages into values that `core`/`domain` can express, into
`Result<T>` and `Status`. `modules/controller/manifest.txt` restricts the whole module to Windows;
its public dependencies are only `core` and `domain`, while Win32, D3D11, DXGI, DWM, NTDLL, and the
Windows Runtime libraries are all Windows private dependencies.

The module owns five groups of responsibilities:

- `modules/controller/source/controller/discovery.hpp`: enumerates top-level windows and collapses
  the volatile Win32 query results into a `TargetCandidate`.
- `modules/controller/source/controller/target.hpp`: uniquely resolves candidates by `TargetSelector`
  and uses `ResolvedTarget` to manage target continuity and `TargetGeneration`.
- `modules/controller/source/controller/capture.hpp`: wraps an already-bound window into a
  `WgcCaptureSession` and returns a BGRA8 `Frame` cropped to the client area.
- `modules/controller/source/controller/input.hpp`: delivers mouse, keyboard, and text only through
  target window messages, maintaining pressed state and per-message auditing.
- `modules/controller/source/controller/dpi.hpp`: establishes and validates the
  `PER_MONITOR_AWARE_V2` process precondition before physical window geometry is used.

Public types live in `uf`, pure-logic implementation details in `uf::controller_detail`, and the
Win32 bridge in `uf::controller_platform`. There is no `uf::controller` namespace in the repository;
new code that guesses the namespace from the directory name would bypass the existing organization.

controller deliberately does not own the following policies:

- It does not recognize pages, interpret `Detection`, or authorize actions; those belong to
  `annotation` and `engine`.
- It does not decide how the product fuzzy-matches by title, whether hidden windows are ignored, or
  how ambiguity is presented to the user. controller itself provides only precise selectors and
  "must be unique" resolution; the CLI and workbench can implement their own policies on top of the
  candidate values.
- It does not allocate product-level `CaptureSessionId`, nor manage task cancellation, retries, trace files,
  or the Luau lifecycle.
- It does not validate the project's `ProjectFingerprint`. `TargetIdentity` does not include DPI, and
  `DeliveryTarget` does not carry a compatibility proof.
- It does not provide any fallback path for foreground activation, global input, or moving the real
  cursor. When the target does not accept `PostMessageW`, the call fails rather than switching
  backends.
- It does not abstract a second platform. The cross-platform call surface is `engine`'s port; this
  module itself is intentionally Windows-only.

This boundary keeps the offline-reproducible authorization semantics in platform-independent modules
while concentrating all HWND, COM, callback, and driver lifetimes into one auditable area.

## Targets, Capture, and Input

### Discovery, resolution, and target generations

The discovery entry point is `enumerateCandidates() -> Result<std::vector<TargetCandidate>>`. The
actual path in `modules/controller/source/controller/platform/windows-discovery.cpp` is:

1. `EnumWindows` synchronously collects `WindowHandle`s; the callback only stores non-owning opaque
   tokens.
2. Each handle is queried in turn for its PID, optional process start time, optional executable path,
   window class, title, client size, DPI, and visible and iconic state.
3. When `OpenProcess` hits access denied or the process exits during the query, only the start time
   and path are degraded to empty; if a required fact such as PID, class, title, client size, or DPI
   fails to query, it returns `TargetUnavailable`.
4. A `GetWindowThreadProcessId` failure and `GetDpiForWindow == 0` are both explicitly rejected;
   after each intermediate query failure the code rechecks whether the window has already
   disappeared, in order to distinguish a normal race from a genuine API failure.

`WindowHandle`, `ProcessId`, `ProcessStartTime`, and `Dpi` are strongly typed values, and
`ClientSize` holds an unsigned width and height. A `TargetCandidate` is a single enumeration snapshot,
not a continuously valid window borrow; its optional metadata must not be misread as a confirmed
identity.

`TargetSelector`'s `withProcess`, `withWindowHandle`, `withWindowClass`, and `withTitle` return new
values, and string matching is exact and case-sensitive. `matchingCandidates` only filters;
`resolveTarget` additionally requires exactly one match. Both zero matches and multiple matches return
`TargetUnavailable`, and when both a PID and an HWND are specified but conflict, a separate error is
reported, so resolution never "picks the first window that looks suitable".

`ResolvedTarget` stores `TargetIdentity { WindowHandle, ProcessId, ProcessStartTime?, ClientSize }`,
the current `TargetGeneration`, and internal continuity. The initial generation is zero.
`revalidate()` re-reads the live identity and hands it to `applyRevalidation`:

- When the PID and start time are both identical, and the HWND and client size are also identical:
  returns `RevalidateOutcome::Unchanged`.
- When the process instance differs, or the HWND/client size of the same process instance changes:
  the generation advances once, the new identity is stored, and it returns `GenerationBumped`.
- When the window disappears: the generation advances once and latches `Lost`.
- When either side lacks a process start time: PID reuse cannot be ruled out, so the generation
  advances once and latches `InstanceUnconfirmed`.

`Lost` and `InstanceUnconfirmed` do not repeatedly advance the generation afterward; recovery must
come from `reResolve` with a new candidate. An explicit `reResolve` on a still-confirmed target first
advances the generation; on a target that has already latched invalid it does not advance again. This
makes each continuity break produce exactly one generation boundary. Both `TargetGeneration::next()`
and `FrameIdCounter::nextId()` refuse to wrap, so old credentials do not become valid again through
integer overflow.

`errorOnLost` only converts `Lost` into `ControllerDisconnected`; other outcomes are still handled by
the caller according to context. `ResolvedTarget`'s multiple Win32 reads are not an atomic snapshot,
and the equal-value HWND recycling race remains a registered open item.

### WGC session and frames

The caller first uses `ClientGeometry::create` to supply the physical desktop-space client origin and
a positive client extent, then calls `WgcCaptureSession::create(WindowHandle, CaptureSessionId,
TargetGeneration, ClientGeometry, WgcCaptureOptions)`. The session permanently retains the
session/generation it was created with; after the target changes it should be destroyed and rebuilt
with a new generation rather than mutated in place.

The creation process lives in
`modules/controller/source/controller/platform/windows-capture.cpp`:

1. `WindowInstanceMarker` sets a window property with a process-unique name on the target HWND; the
   property value is a session-exclusive event-handle token. If an HWND with the same numeric value
   is recycled by the system, the new window does not inherit that property, so `matches()` can reject
   the wrong instance.

   **Win32 error 5 at this `SetPropW` means elevation, and since 2026-07-30 (`2429578`) the message
   says so** instead of reporting the bare number. Stamping a property on the target is refused by
   UIPI when the target runs at a higher integrity level than the caller. The obvious probe points
   the wrong way — `PostMessage` to the same window succeeds unelevated — so input delivery working
   does **not** imply capture will bind, and that cost a real detour. Error 5 here has one cause, so
   the message names it; other Win32 errors keep the numeric form. See
   `docs/pitfalls/capture-and-target-selection.md`.
2. Checks WGC availability and the OS build. Below build 19041, cursor capture cannot be turned off,
   so the session fails outright. `WgcCaptureOptions::requireBorderless()` currently always fails,
   because there is no caller-owned borderless access grant path yet.
3. Creates a hardware D3D11 device, falling back to WARP on failure. The immediate context enables
   `ID3D11Multithread` protection, because the free-threaded WGC callback shares the device with the
   consumer side.
4. `CreateForWindow` builds the `GraphicsCaptureItem`, reads the initial item size, and computes the
   `ClientCropRect` from `ClientToScreen(0, 0)` and `DWMWA_EXTENDED_FRAME_BOUNDS`. It does not use
   `GetWindowRect` here, to avoid counting the invisible resize border that WGC does not include into
   the crop.
5. Creates a two-buffer free-threaded frame pool, turns off cursor capture, registers the item-closed
   and frame-arrived callbacks, and then calls `StartCapture`.

The frame-arrived callback does not capture `Impl` or a bare `this`; it only holds a
`std::shared_ptr<FrameSlot>` by value. `FrameSlot` protects a single `m_latest` with a mutex: the
producer overwrites it with the latest `CapturedArrival`, and the consumer clears it after taking it,
so a slow consumer does not accumulate an unbounded frame queue. The callback records the host arrival
time with `MonotonicInstant::now()`, not WGC's GPU produce time. Item closed and callback HRESULT
failures are also published to the same wait predicate.

`WgcCaptureSession::capture()` executes serially under the operation mutex:

1. `waitForFrame` waits for the latest frame, item closed, callback failure, or a stall timeout.
   `StallTracker` judges freshness by arrival time rather than by pixel change or consumption time;
   even if a frame is already in the slot, if the timeout is exceeded at consumption time it still
   returns `CaptureStalled`. `StallTracker::check` requires a `TargetWindowState` alongside the
   instant, because a minimized or destroyed window composites nothing and is therefore the *cause*
   of the stall rather than an unrelated fact. `observeTargetWindow` (in `windows-capture.cpp`,
   `IsWindow` then `IsIconic`) supplies it, and `stalledFrameFailure` turns it into a message that
   names the window state and the action that clears it. Occlusion and an off-screen position are
   deliberately not probed: DWM keeps composing for both, so neither can be the cause.
2. `CaptureGeometryState::observeContentSize` requires every frame's `ContentSize` to be exactly the
   same as at creation, and the D3D surface size must also equal the confirmed size. Any invalid value
   or mismatch permanently latches invalidated, after which the session must be rebuilt even if the
   size later recovers.
3. `readbackSurface` copies only the `ClientCropRect` into a reusable staging texture, and
   `readbackBgra8` strips the D3D row-pitch padding to produce tightly packed BGRA8 client pixels.
4. Revalidates the `WindowInstanceMarker` and re-reads the current client origin. When the window only
   moves while its extent stays unchanged, the pixel dimensions remain stable, but the new
   `CoordinateTransform` reflects the new desktop origin.
5. Allocates a monotonic `FrameId`, uses the callback arrival time as `capturedAt`, and places the
   immutable `std::shared_ptr<FrameBuffer const>`, the session/generation, and the transform together
   into the `Frame`.

`CaptureHygiene` exposes the OS build at creation time, whether the cursor is disabled, borderless
support, and the border-required state, so that upper layers can record capability facts.
`validateTargetInstance()` separately checks item-closed and the marker, for engine to call at the
observe and delivery edges.

`close()` and `capture()` share the operation mutex. Teardown first sets
`FrameSlot::m_acceptingFrames` to false, then closes the session, revokes the callbacks, closes the
frame pool, and clears the latest frame and the window property. An in-flight callback that sees
accepting=false closes its frame and does not refill the slot.

### Strict-background input

`DeliveryTarget::create` fixes `WindowHandle`, `CaptureSessionId`, `TargetGeneration`, and a non-empty
`ClientSize` into a single delivery capability. It does not own the window, nor does it automatically
follow `ResolvedTarget`; when the generation or session changes, a new value should be created.

The public entry points for coordinate actions are `movePointer`, `click`, `pointerDown`,
`pointerUp`, `longPress`, and `scroll`. They accept a `Point<ClientSpace>` and an `ObservationLease`.
The actual rejection order of `checkPointerPreconditions` is:

1. the lease session must equal `DeliveryTarget::sessionId()`;
2. the current monotonic time must not be later than the lease expiry;
3. the lease generation must equal `DeliveryTarget::generation()`;
4. the coordinates must be finite, lie within the half-open client bounds, and be encodable as a
   non-negative signed-16-bit mouse coordinate;
5. the floating-point coordinates are turned into a `ClientPixel` with `floor`.

Then `HeldInputs` must not be bound to another delivery identity, and `IsWindow` must still be true,
before entering the message boundary. `click` delivers move, left-down, and left-up in sequence; if
the down succeeds but the up fails, the held state is deliberately retained for the caller to
compensate. `longPress` waits after the down, then, through a caller-provided refresh callback,
requires the HWND, session, and generation to be all unchanged before delivering the up; a change in
client geometry itself does not affect this identity comparison.

`scroll` posts one `WM_MOUSEWHEEL` at that point, and it is the only entry point here whose `lParam`
is in **screen** coordinates: Win32 documents the wheel message's position that way, while
`WM_MOUSEMOVE` and the button messages are in client coordinates. It therefore translates the
`ClientPixel` by `ClientToScreen(hwnd, {0, 0})` into a `controller_detail::ScreenPixel` before
building the message. That is a separate type on purpose — a screen coordinate is negative on any
monitor left of or above the primary one, which `ClientPixel` refuses by construction, so the two
spaces cannot be passed interchangeably to a message builder. It still runs the full
`checkPointerPreconditions` fence, because the wheel's position is what the target hit-tests to
decide which control scrolls. `WheelDelta` counts notches of `WHEEL_DELTA` (120), positive away from
the operator, refuses zero, and is bounded so the raw delta still fits the signed 16-bit high word of
`wParam`. Vertical only: `WM_MOUSEHWHEEL` is deliberately absent, because it would need its own axis
in every layer down to the wire and nothing in this project scrolls sideways.

The keyboard entry points `keyPress`, `keyDown`, `keyUp`, `inputText`, and `inputUnichar` do not
accept an `ObservationLease`; they only compare the action generation. `KeyInput` records the virtual
key and the extended-key bit; `inputText` first strictly decodes UTF-8, then sends `WM_CHAR` per
UTF-16 code unit, while `inputUnichar` accepts only a Unicode scalar and sends `WM_UNICHAR`.

**`keyPress` acquired its first production caller on 2026-07-30** (`ed38124`). Until then the whole
keyboard path was exercised only by tests, and the "generation, not lease" shape above was a
capability nobody used; it is now the contract `engine::IActionSink::pressKey` and the CLI's
`ControllerActionSink` are built on, and it turned out to be exactly right — a keystroke names no
coordinate, so there is no rect a lease could fence. What was missing was only the name-to-virtual-key
mapping: `KeyInput::fromName` and `KeyInput::fromKeyName` resolve one, and **which names exist is
`KeyName`'s single definition in `modules/domain`**, which `fromName` routes through rather than
repeating, so the two cannot come to disagree. The other four entry points still have no production
caller.

`fromKeyName` resolves three families, each as a rule rather than a comparison chain: the named keys
through a `NamedKeyCode` table, `"F1".."F12"` through `VK_F1 + n - 1` since those codes are
consecutive, and a single letter or digit through its own ASCII code, since `VK_A..VK_Z` and
`VK_0..VK_9` are defined as exactly that. The named table is paired with `domain::k_namedKeys` by a
`static_assert`, so a name admitted in `domain` with no virtual key here fails the build rather than
reaching the single-character branch and tripping its `UF_CHECK` on a keystroke the author was
entitled to write. That guard is what keeps `fromKeyName` `noexcept` and total. `"ENTER"` is
`VK_RETURN` and **not** extended; the extended numpad Enter remains its own named factory,
`KeyInput::numpadEnter()`.

One consequence of admitting `"SHIFT"` is worth stating: `wheelSpec` still reports only the left
button in the wheel's `wParam`, never `MK_SHIFT`. A held modifier has become expressible, but
reaching that state needs `keyDown`, which has no production caller, so no wheel this project posts
can be missing a modifier it should carry. Derive one there the day `keyDown` acquires a caller.

`modules/controller/source/controller/detail/input-message.hpp` encodes action determinism into a
`PostSpec`: the mouse uses only `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`, and `WM_LBUTTONUP`, and the keyboard
uses only `WM_KEYDOWN`, `WM_KEYUP`, `WM_CHAR`, and `WM_UNICHAR`. The one final system call lives in
`postInputMessage` in `modules/controller/source/controller/platform/windows-input.cpp`. It first
rejects a null HWND and `HWND_BROADCAST`, then writes the attempt to the `AuditLog`, and finally calls
`PostMessageW`. An audit record therefore represents an "attempted delivery" and includes an ordinary
HWND that subsequently fails; null/broadcast leaves no record because no attempt is made.

`HeldInputs` uses an ordered set/map to store held keys and the pointer, bound to
`{ HWND, CaptureSessionId, TargetGeneration }`. `releaseHeld` always clears the in-memory state first, then
best-effort delivers the Up in the stable order of keys before pointer, returning a `ReleaseOutcome`
for each item. When the identity does not match, nothing is delivered at all. Clearing first
guarantees that this process does not carry an old held capability to a new target, but it also means
that a failed Up has no built-in retry state.

`AuditLog::records()` returns a span borrowing the internal vector; the next audited delivery may grow
it and invalidate it. Both `HeldInputs` and `AuditLog` are caller-owned, and controller does not hide
any global input state.

### DPI

`ensurePerMonitorAwareV2()` calls `SetProcessDpiAwarenessContext`, then validates the actual state
using the current thread context. A successful set returns `DpiDeclaration::Declared`; only when Win32
returns access denied and the actual context is already V2 does it return `AlreadyDeclared`. A setter
that appears to succeed but whose actual context is wrong, an access denied with a wrong context, or
any other Win32 error all fail closed with `InternalInvariant`.

This call should happen before any window query that depends on physical pixels. It establishes only
the process coordinate precondition; per-window DPI is obtained by discovery's `GetDpiForWindow`, and
the continuous validation of the project-level DPI fingerprint is not within this API.

## Constraints That Must Remain True

### Fail-closed and generation isolation

- Target selection must be unique; an unconfirmable identity, a lost window, a size change, a WGC item
  closed, a callback failure, a stale arrival, an invalid crop, or a surface mismatch all return a
  structured failure, with no guessing, no cropping, and no reuse of old geometry.
- `Lost`, `InstanceUnconfirmed`, and capture geometry invalidation all latch. A transient recovery
  cannot automatically revive an old lease or an old transform.
- pointer delivery revalidates the lease session, age, generation, client bounds, held binding, and
  window liveness before `PostMessageW`. This layer currently has no "current FrameId" input, so it
  does not compare the lease frame ID; the full frame-ID comparison happens in `authorizeCoordinateAction`
  in `modules/annotation/source/annotation/authorization.cpp`. These two layers must not be described
  together as controller independently performing full four-field fencing.
- When the input API is called on its own, `IsWindow` can only prove that the value is currently a
  window, not that it is still the original instance. The product path relies on `EngineSession::act`
  running `WgcCaptureSession::validateTargetInstance` immediately before the sink call, and then the
  input layer performs the above lease checks.

### Determinism

- The selector's string comparison, the candidate-count rule, the coordinate `floor`, the Win32
  `lParam` bit layout, and the release ordering are all explicit rules; there is no fuzzy match, no
  random window selection, and no implicit rounding.
- `TargetGeneration` and the in-session `FrameId` are monotonic and non-wrapping. The same input state
  produces only `Unchanged` or exactly one generation bump.
- `FrameSlot` defines latest-frame coalescing explicitly; `StallTracker` uses a monotonic arrival
  timestamp. Real desktop frames and scheduling timing are not themselves reproducible inputs, but
  given an arrival sequence, the accept/reject conditions are deterministic.
- `AuditLog` preserves the attempt order and a monotonic timestamp. It is diagnostic evidence, not a
  success confirmation; a successful `PostMessageW` only means the message has been enqueued.

### Ownership, lifetime, and concurrency

- `WgcCaptureSession` is non-copyable and exclusively owns the COM session, frame pool, D3D state, and
  marker through a `std::unique_ptr<Impl>`. The owned process HANDLE uses `UniqueProcessHandle`, COM
  interfaces use `winrt::com_ptr`/projection, and the window marker token uses `winrt::handle`.
- The asynchronous callback holds only a shared `FrameSlot`, not a session borrow or a bare `this`.
  The slot mutex protects latest/accepting and a condition variable handles publication; the
  operation mutex serializes consumer-side geometry, stall, D3D, and teardown.
- The raw span from the D3D Map completes its copy before the paired Unmap and does not escape the
  platform function. The returned `Frame` shares an immutable pixel buffer and can safely outlive the
  session's next capture.
- `close()` currently cannot interrupt a `capture()` wait that already holds the operation mutex; a
  concurrent close waits until the capture timeout. This is a known boundary, and RAII teardown must
  not be mistaken for a cancellable wait.
- `HeldInputs`, `AuditLog`, and `DeliveryTarget` are all explicit caller-owned values; controller has
  no hidden singleton. The span returned by `AuditLog` is only valid until the next append.

### Strict-Background and the Platform/FFI Boundary

Strict-background is not a runtime switch but a set of reachable APIs. `k_forbiddenBackgroundApis`
records six original guard names; `scripts/check_safety.py` also statically forbids the direct use of
other foregrounding APIs. controller's only injection primitive is `PostMessageW`; it never calls
focus, activation, global input, or cursor-position APIs, and it has no post-failure fallback branch.

All `Windows.h`, D3D, DWM, WinRT ABI, native out-parameters, opaque-handle bit conversions, and mapped
pointers live in `modules/controller/source/controller/platform/`. Every dangerous operation has a
nearby `// SAFETY:` and is converted within the boundary into a value, a RAII owner, or a `Result<T>`.
Public headers do not expose `HWND`, `HANDLE`, COM pointers, or Windows SDK structs; `WindowHandle` is
merely a pointer-width, non-owning opaque value.

This module has no separate `ffi/` directory: the FFI responsibility for Win32 and COM is fully
carried by `platform/`. `modules/controller/source/controller/detail/` holds portable pure algorithms
and narrow access helpers, whose main purpose is to make the boundary rules offline-testable rather
than to provide a second public API.

## Dependencies

Downward, controller crosses only two module edges:

- `core` provides `Result`/`Status`, contracts, checked arithmetic/cast, strong values, non-wrapping
  generation, monotonic time, and scope-exit.
- `domain` provides `Frame`, `FrameBuffer`, `CaptureSessionId`, `TargetGeneration`, `ObservationLease`,
  coordinate spaces, `CoordinateTransform`, and `AutomationErrorKind`. controller produces or consumes
  these values but does not redefine their semantics.

Upward, the current product composition happens in `entry/cli/run-windows.cpp`:

1. `ensurePerMonitorAwareV2` establishes the DPI precondition;
2. after `enumerateCandidates`, the CLI does title-substring selection and then `resolveTarget` by
   exact HWND;
3. the candidate's client size/DPI constructs a one-time live fingerprint, and the entry's
   `clientOriginDesktop` constructs the initial `ClientGeometry`;
4. the same HWND/session/generation creates the `WgcCaptureSession` and the `DeliveryTarget`;
5. `WgcFrameSource` in `entry/cli/platform/wgc-frame-source.hpp` maps capture and marker validation
   onto `engine::IFrameSource`;
6. `ControllerActionSink` in `entry/cli/platform/controller-action-sink.cpp` owns the
   `DeliveryTarget`, `HeldInputs`, and `AuditLog`, passes the lease through to `uf::click` unchanged,
   and after a click failure calls `releaseHeld` while preserving the original error.

`modules/engine/source/engine/session.cpp` validates the frame source before observe, and revalidates
the target instance after action authorization but before the sink call. annotation then checks the
fingerprint, page, detection, full frame identity, and lease, and controller finally checks the
session/generation/age/bounds observable at the delivery point. The only data crossing the boundary
are domain values and structured errors; no HWND or D3D resource enters engine.

`entry/workbench/platform/windows-capture-source.cpp` reuses discovery and `WgcCaptureSession` for a
one-shot authoring source capture and then hands the `Frame` to source ingestion. Its
visible/non-iconic and "first title substring match" policy belongs to workbench, not to controller's
resolution contract.

`entry/input-agent/` and `entry/m0-demo/` use the target, capture, and input surfaces directly.
The agent is the annotation front-end (see [`entry-input-agent.md`](entry-input-agent.md)); the demo
is frozen as a real-machine acceptance reference, and its product path is superseded by the
engine/CLI composition. The
low-level `AuditLog` records Win32 message attempts, while the engine `ITraceSink` records product
events such as observe/authorize/deliver; the two serve different purposes and cannot substitute for
each other.

## Tests

`tests/CMakeLists.txt` registers `test-controller` only when the `${PROJECT_NAME}_controller` target
exists, so these tests are Windows-only, but OS-independent rules are extracted and run offline as far
as possible.

- `tests/controller/test-discovery.cpp` pins down pointer-width handles, failed live Win32 queries,
  best-effort process metadata, FILETIME assembly, and loss-tolerant UTF-16 conversion.
- `tests/controller/test-target.cpp` pins down exact/case-sensitive selectors, zero/multiple-match
  rejection, PID/HWND conflict diagnosis, the single generation bump on identity change,
  `Lost`/`InstanceUnconfirmed` latching, PID reuse, and explicit re-resolution.
- `tests/controller/test-dpi.cpp` exhaustively covers the DPI setter classifications: success, access
  denied, actual-context mismatch, HRESULT low bits, and other errors.
- `tests/controller/test-capture-wgc.cpp` covers `FrameIdCounter`, `ClientGeometry`, the whole-pixel
  client extent, `CaptureGeometryState`'s ContentSize latch, and the `WgcCaptureOptions` timeout.
- `tests/controller/test-capture-stall.cpp` pins down the timeout boundary, arrival-time freshness,
  and the reset on a new arrival; `tests/controller/test-capture-os-build.cpp` pins down the
  19041/20348 capability threshold.
- `tests/controller/test-capture-readback.cpp` covers DWM/WGC crop geometry, far-edge bounds, overflow/zero
  rejection, and padded-row BGRA8 packing.
- `tests/controller/test-input-message.cpp` pins down keyboard/mouse message bits, extended keys,
  UTF-8→UTF-16, signed-16-bit coordinates, auditing before a failed delivery, and zero delivery for
  null/broadcast.
- `tests/controller/test-input-revalidation.cpp` pins down the check order of the pointer fence, the
  lease session/expiry/generation, half-open bounds, the finite/floor rules, the signed-16-bit
  limits, keyboard generation, and a dead HWND.
- `tests/controller/test-input-held.cpp` pins down the held binding, the stable Up list,
  clear-before-attempt, and zero posts on mismatch; `tests/controller/test-input.cpp` covers empty
  delivery geometry, dead-target compensation, refresh identity, and long-press precondition
  rejection.
- `tests/controller/test-audit-log.cpp` pins down the runtime forbidden-name list and the `AuditLog`
  append order; the static source-code ban is enforced separately by `scripts/check_safety.py`.
- `tests/engine/test-session.cpp` pins down, from the other side of the port, delivery-edge target
  revalidation, lease pass-through, and a sink click count of zero on the various authorization
  failures.

These unit tests do not create a real `WgcCaptureSession`, nor do they directly drive
`WindowInstanceMarker`, the free-threaded callback, the D3D driver, or successful real window input.
resize, recreation, minimize/stall, DPI, occlusion, and focus invariance still require real-machine
verification; `docs/plans/2026-07-20-m0-demo-port-deviations.md` positions the frozen m0-demo as that
acceptance reference rather than a CI substitute.

## Future Extensions

The following seams have a plan basis and do not imply that the capability already exists.

The first group is discovery/identity hardening. S-1/S-2 of
`docs/plans/2026-07-20-post-port-win32-robustness.md` note that `probeCandidate` and
`ResolvedTarget::readLiveIdentity` are still multiple, non-atomic HWND queries. `WindowInstanceMarker`
only protects an already-established capture session and cannot fix the enumeration stage stitching
two windows' fields into one snapshot. Subsequent atomicization or a post-observation identity recheck
should be attached at the platform probe and the live revalidation, preserving the fail-closed
semantics of `InstanceUnconfirmed`. S-4's dynamic title/class buffer also belongs to the same
discovery boundary.

The second group is the continuous compatibility gate. §2 of
`docs/plans/2026-07-22-annotation-design.md` and D8 of
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` require recognition and Controller delivery
to continuously re-check the live size, integer DPI, target identity, and transform beforehand.
Currently `ResolvedTarget` does not store the DPI, the CLI's live fingerprint is constructed only at
startup, and `DeliveryTarget` has no compatibility proof. A new fresh observation must be wired into
`IFrameSource::validateTargetInstance` and the delivery edge; a mismatch should advance the generation
or return `TargetCompatibilityUnverified`, and must not merely update the `CoordinateTransform` and
keep using the old lease. P1's base-to-live adaptive transform should remain a separate value of
annotation/runtime and should not be crammed into controller's existing live Client↔Frame
`CoordinateTransform`.

The third group is capture liveness. The Win32 robustness plan still lists occlusion determination,
capture-wait cancellation, and pairing the stall timeout with the lease age as open items. A shared
signal can be wired into `FrameSlot`'s wait predicate and notification;
`WgcCaptureOptions::captureStallTimeout()` remains a capture-side knob, while the action age is owned
by `ObservationLease`/engine configuration, and the two need to be coordinated at the composition
layer. Any optimization must preserve arrival-time freshness, item-closed/marker rejection, and the
ContentSize latch, and must not simply treat a static picture as a safe new frame.

The fourth group is input compensation. The same Win32 plan records three real seams: `releaseHeld`
may re-check the window instance before each best-effort Up; clear-before-attempt may evolve into an
explicit retry/persisted-held policy; and when `scanCodeFor` returns zero, it may reject with
`ActionRejected` before constructing the `PostSpec`. `ControllerActionSink` is already the central
point of product compensation, but identity proof and retry ownership must still be explicit, and a
failed Up must not be silently counted as a success.

The fifth group is DPI and borderless capability. A stronger process-DPI validation could replace the
calling-thread-context-based confirmation in
`modules/controller/source/controller/platform/windows-dpi.cpp`; the existing call still requires
executing before any thread DPI override. borderless capture already has a fail-closed switch reserved
by `WgcCaptureOptions::requireBorderless`, and a real implementation must first establish a
caller-owned access grant and then change the session setting; it must not report borderless as
fulfilled based solely on OS build support.
