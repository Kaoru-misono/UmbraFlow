# `modules/domain` Architecture Knowledge

`domain` is UmbraFlow's platform-independent semantic layer: it encodes "which frame, which target
instance, which coordinate space, is this action still trustworthy" into values that can be passed
across modules. It does not perform recognition, window management, or input delivery; it provides
the vocabulary and rejection rules that these flows depend on in common and cannot each interpret on
their own.

## Module Scope

`modules/domain/manifest.txt` exposes a dependency only on `core`. `domain` can therefore be used in
common by `vision`, `image`, `annotation`, `engine`, `script`, and the Windows-only `controller`,
without dragging platform APIs or higher-level product policy back down into the lower layer.

It owns five groups of contracts:

- `modules/domain/source/domain/space.hpp` and `space.cpp` define coordinate spaces, floating-point
  geometry, integer pixel geometry, and the provable bridge between the two.
- `modules/domain/source/domain/ids.hpp` and `ids.cpp` define strongly typed identities, `Label`,
  and the non-wrapping `TargetGeneration`.
- `modules/domain/source/domain/frame.hpp` and `frame.cpp` define `Frame`, which carries identity,
  capture time, pixel ownership, and the coordinate transform.
- `modules/domain/source/domain/detection.hpp` and `detection.cpp` define the same-frame `Detection`
  and the action-timeliness credential `ObservationLease`.
- `modules/domain/source/domain/error.hpp`, `error.cpp`, `time.hpp`, and `time.cpp` define
  automation error classification, recovery scope, and safe arithmetic over monotonic time.

`domain` deliberately does not own the following responsibilities:

- It does not produce `CaptureSessionId` or `FrameId`. `CaptureSessionId` is provided by the composition root and
  is currently constructed by the CLI in `entry/cli/run-windows.cpp`; the per-capture allocation of
  `FrameId` is done by `FrameIdCounter` in
  `modules/controller/source/controller/detail/capture-wgc.hpp`.
- It does not decide when a target window has changed generation.
  `ResolvedTarget` in `modules/controller/source/controller/target.cpp` advances
  `TargetGeneration` based on the process instance, window handle, client size, and continuity.
- It does not interpret whether a detection label can trigger an action. `Detection` is merely
  geometric evidence bearing same-frame identity; only `ActionDetection` and
  `authorizeCoordinateAction` in `modules/annotation/source/annotation/authorization.cpp` bind the
  catalog, recognizer, page, and live fingerprint.
- It does not perform template matching, cropping, PNG codec work, trace, or retries. Those policies
  belong respectively to `vision`, `image`, `engine`, or the caller.
- It does not implement strict-background. Background delivery is implemented at the `PostMessageW`
  boundary in `modules/controller/source/controller/platform/windows-input.cpp`; the list of APIs
  forbidden for foregrounding and global injection lives in
  `modules/controller/source/controller/input.hpp`.
- It does not attach implicit validation to arbitrary `Point`, `Rect`, or `Detection` constructors.
  A boundary that needs safety guarantees must explicitly call `create`, `ensure...`, or an
  authorization function, and must not mistake "the type exists" for "the value has been validated".

Such boundaries keep `domain` "small but load-bearing": the mechanisms can be tested offline and
reused across platform code, while platform facts and product authorization are still judged by the
modules that actually hold the context.

## Shared Data Model

### Coordinate Spaces and Geometric Values

`modules/domain/source/domain/space.hpp` declares four empty tags:

- `DesktopSpace`: desktop coordinates; the client origin is also expressed in this space.
- `ClientSpace`: coordinates in the target window's client area, the coordinates in which the
  Controller accepts pointer actions.
- `FrameSpace`: floating-point coordinates on the captured frame, the common space in which
  detection rectangles and recognition results enter the action chain.
- `NormalizedSpace`: floating-point coordinates normalized by the frame width/height.

The `CoordinateSpace` concept restricts `Point<Space>` and `Rect<Space>` so they can only be
instantiated over these four spaces. The template parameter makes `Point<DesktopSpace>` impossible to
pass by mistake to an API that requires `Point<ClientSpace>`; this is compile-time dimensional
isolation, not a runtime range check.

`Point<Space>` holds only two `float`s. `Rect<Space>` holds `x`, `y`, `width`, and `height`;
`contains` uses the half-open interval `[x, x + width) × [y, y + height)`, `center` computes the
floating-point center, and `isEmpty` treats any non-positive extent as empty. The constructor allows
negatives, infinities, and NaN, so a safety boundary must validate separately.

`PixelPoint` and `PixelRect` use `uint32` to express discrete pixels rather than a particular
desktop space. `PixelRect::create`:

1. rejects zero width or zero height;
2. uses `checkedAdd` to prove that `x + width` and `y + height` do not overflow;
3. caches `right` and `bottom` so later extent checks need not repeat unsafe addition.

`PixelRect::ensureWithinExtent` returns `ActionRejected` when out of bounds; it does not clip.
`PixelRectHash` computes a stable value hash over the four value fields for use in unordered
containers; it is not a persistence hash or a content identifier.

### The Exact Integer–Float Bridge

`k_maxExactFrameDimension` equals `1 << 24`, that is `16,777,216`. IEEE-754 binary32 can exactly
represent every integer in the closed interval `[0, 2^24]`, while `2^24 + 1` is the first integer
that cannot be represented exactly. This inclusive bound is an interface contract and must not be
rewritten as `< 2^24`.

`pixelPointToFramePoint` checks per coordinate that `x` and `y` do not exceed that upper bound before
it does `static_cast<float>`. Out of range it returns `InvalidResource`, because the input pixel
resource can no longer enter `FrameSpace` losslessly.

`pixelRectToFrameRect` checks the far edges `x + width` and `y + height`, promoting to `uint64` first
to avoid wraparound near the `uint32` upper bound. A far edge equal to `2^24` is legal; only
exceeding it returns `InvalidResource`. After the check passes, the origin, extent, and far edges all
lie within the exactly representable range, so the four integer-to-`float` conversions lose no
information.

The reverse direction `CoordinateTransform::frameRectToPixelRect` is deliberately a coverage-style
quantization, not a per-value inverse transform:

- it first uses `ensureFrameRectInBounds` to prove the rectangle is finite, non-empty, and within
  the frame;
- the start takes `floor` and the far edge takes `ceil`, so any rectangle with area covers at least
  one pixel;
- for up to `1e-3F` of boundary floating-point error, it first clamps to the true frame extent;
- it uses `checkedIntegralCast<uint32>` and `checkedSubtract` to construct the final `PixelRect`;
- if no pixel is covered after quantization, it returns `ActionRejected` rather than guessing a
  result.

So the exactness promise is: a legal integer `PixelRect` can round-trip through
`pixelRectToFrameRect` and then through `frameRectToPixelRect` on a same-sized identity transform;
converting an arbitrary floating-point rectangle to pixels was always going to quantize according to
the coverage rule.

### `CoordinateTransform`

`CoordinateTransform::create` is the only public construction entry point. It requires the desktop
client origin and client width/height to be finite, the client extent to be positive, and the frame
width/height to be non-zero and each not exceeding `2^24`. Failure is classified as
`InternalInvariant`, because the caller is trying to establish a transform that the system cannot
safely use internally.

The object stores:

- `m_clientOriginX/Y`: the position of the client top-left corner in `DesktopSpace`;
- `m_clientWidth/Height`: the client's floating-point extent;
- `m_frameWidth/Height`: the captured frame's integer extent.

Point transforms consist of translation and per-axis scaling: `desktopToClient`/`clientToDesktop`
handle only the origin; `clientToFrame`/`frameToClient` scale by the ratio of client to frame size;
`frameToNormalized`/`normalizedToFrame` then divide by or multiply by the frame extent.
`desktopToFrame` and `frameToDesktop` are simply compositions of the steps above.

The rectangle transforms `clientRectToFrame` and `frameRectToClient` apply the same ratio to the
origin and extent and do not clamp. All of these pure transforms are `noexcept` and assume the
transform itself has already been validated by `create`; they do not verify that an incoming point is
finite or within the visible region, nor do they force `NormalizedSpace` to fall within `[0, 1]`.

Action boundaries use two different rules:

- `ensureFrameRectInBounds` allows `1e-3F` of boundary floating-point error, suitable for a
  rectangle that is scaled and then quantized.
- `ensureFramePointInBounds` requires a strict half-open range `0 <= x < width`, `0 <= y < height`,
  with no epsilon, suitable for a final click point.

### Frame Identity and Pixel Ownership

`modules/domain/source/domain/ids.hpp` uses `StrongId<Tag>` to define `EngineRunId`, `TaskRunId`,
`CaptureSessionId`, `FrameId`, `StateId`, `RecognitionId`, and `ActionId`. The same `uint64` value cannot be
implicitly converted between these types, so an identical "17" in a log does not imply identical
semantic identity.

`TargetGeneration` wraps `core::Generation`. Its default value is the same as `initial()`;
`fromValue` is mainly for recovery or testing; `next()` returns `InternalInvariant` at the `uint64`
apex and never wraps around to re-accept stale evidence.

The meaning of D0's dual counters is locked by
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md`:

- `FrameId` is the high-frequency liveness dimension. The current WGC `FrameIdCounter::nextId`
  allocates an increasing ID before each successful frame assembly, and failing the counter overflow
  is a failure.
- `TargetGeneration` is the low-frequency safety dimension. `ResolvedTarget` advances it when the
  process instance changes, the handle/client size changes, continuity is lost, or an explicit
  re-resolve happens; a no-change revalidation of the same target does not advance it.
- `CaptureSessionId` further isolates capture sessions, avoiding collisions between a new session recounting
  from a low `FrameId` and old evidence.

The true frame identity is therefore the triple `(CaptureSessionId, TargetGeneration, FrameId)`. `Frame`,
`Detection`, `ObservationLease`, and annotation's `FrameIdentity` all carry or derive this set of
values.

`FrameBuffer` exclusively owns a single `std::vector<std::byte>` and exposes only `span<const byte>`.
It cannot be copy/move assigned, and there is no mutable byte API for the outside. `Frame` holds a
`shared_ptr<FrameBuffer const>`, so copying a `Frame` or handing pixels to asynchronous recognition
does not copy the whole frame and does not produce a dangling view.

`Frame::create` simultaneously proves:

- the pixel owner is non-null;
- width/height are non-zero;
- `stride >= width * bytesPerPixel(pixelFormat)`;
- `bufferLength >= stride * height`;
- the multiplications and integer conversions above do not overflow;
- `CoordinateTransform::frameSize()` is exactly consistent with the frame width/height.

`PixelFormat` currently has only `Bgra8` and `Gray8`, with `bytesPerPixel` of 4 and 1 respectively.
When adding a format you must extend the default-less switch in lockstep, otherwise an unknown format
should not silently pass the geometry check.

The Windows capture in `modules/controller/source/controller/platform/windows-capture.cpp` records
`capturedAt` in the FrameArrived callback using the host `MonotonicInstant::now()`, then hands the
arrival time, identity, pixels, and current transform together to `Frame::create`. It is not a GPU
produce time, nor a serializable wall clock.

### `Detection` and `ObservationLease`

`Detection` is an immutable value carrier: it holds `CaptureSessionId`, `TargetGeneration`, `FrameId`,
`Label`, `Rect<FrameSpace>`, and `confidence`. The constructor does not validate the rect or the
confidence; a trustworthy action must additionally pass annotation's recognizer/page authorization
and cannot rely on the label alone.

`Label::create` guarantees the string is valid UTF-8 but allows the empty string; `value()` returns a
const reference bound to the owner's lifetime. As recorded in the
[2026-07-28 review follow-up](../../plans/2026-07-28-full-project-review-fixes.md),
`Detection::label()` likewise returns a lifetime-bound `Label const&` instead of copying the label.

`ObservationLease`'s constructor is private and can only be derived from a real `Frame` via
`forFrame`. It copies the frame's identity triple and computes `expiresAt = capturedAt +
effectiveAge`.

`k_defaultMaxActionFrameAge` is 750 ms. `clampMaxActionFrameAge` only allows a caller to shorten this
fuse and cannot loosen it via configuration: `effectiveAge = min(requested, 750ms)`. A negative
duration is rejected before the clamp; `checkedAddMonotonic` also rejects deadline overflow.

The expiry decision is a strict `now > expiresAt`. It is still valid exactly at the deadline; a
zero-duration lease is valid at the instant of `capturedAt` and expires immediately afterward.

`ObservationLease::validate` compares session, target generation, and observed frame in order, and
finally checks age; any failure is `StaleObservation`. It does not retry, because "observe again" is
an upper-layer control-flow choice and should not be hidden inside a safety credential.

### The Error Public Surface

`AutomationErrorKind` enumerates the cause; `FailureResponse` records how far a failure should unwind
outward. The exhaustive switch of `failureResponse(AutomationErrorKind)` currently maps as follows:

- `Cancelled` → `Cancelled`;
- `CaptureStalled`, `StaleObservation`, `RecognitionIncomplete` → `Retry`;
- `ActionRejected` → `StepFailed`;
- all other kinds → `Abort`.

`RecognitionIncomplete` is deliberately not named for a failed recognition. A recognition that
completes and matches nothing is not an error at all — it is `UnknownPage`, or a nil hit, and it
carries no error kind. This kind means only that the comparison budget ran out before the search
finished, so the caller learned nothing about the screen. That is why its response is `Retry` and not
`StepFailed`: the caller has to observe again within its own budget instead of branching as though
the page had been ruled out, and because the comparison count depends on the frame's own pixels, a
later frame can complete where this one ran out.

When adding an `AutomationErrorKind`, the default-less switch forces the developer to choose a
recovery scope. `fail(kind, message, nativeCode)` encodes the kind into the private `uf.automation`
error category; the encoding uses the underlying value plus one, so a legal kind never becomes a
"no error" code with value zero. `automationErrorKind` accepts only that category, so an ordinary
`std::error_code` cannot masquerade as an automation error; an `Error` that cannot be classified is
conservatively mapped to `Abort`.

`systemErrorCode(uint32)` is dedicated to the fidelity of native OS status codes. It deliberately
converts the unsigned bit pattern to `int` before placing it into `std::system_category()`; a
HRESULT-shaped/GetLastError value with the high bit set must not be discarded via checked narrowing.
This function only wraps the native cause; the business kind is still decided by the outer
`fail(AutomationErrorKind, ..., nativeCode)`.

`checkedAddMonotonic` rejects negative durations and time-point overflow. `elapsedNanosecondsSince`
saturates to zero on reversed time and saturates to the maximum on a result that cannot fit in
`uint64`; it is suitable for trace duration and does not take on lease decisions.

## Constraints That Must Remain True

**Fail-closed.** Non-finite geometry, empty/out-of-bounds regions, integer overflow, transform/frame
size inconsistency, stale identity, an expired lease, and unknown error classification all return a
structured failure; the conversion functions do not automatically carve out an action target that
"looks usable". The epsilon on rectangle boundaries only absorbs floating-point noise, and the result
is still clamped to the true extent.

**Identity consistency.** From a single recognition to action authorization,
`(CaptureSessionId, TargetGeneration, FrameId)` must be maintained.
`modules/annotation/source/annotation/authorization.cpp` compares the `FrameIdentity` of the resolved
page, action detection, and delivery state, then calls `ObservationLease::validate`. This makes a
detection "from the same label but a different frame" impossible to authorize.

**The current implementation layering of D0/D1.** The `Observation` in
`modules/engine/source/engine/session.cpp` is move-only; both the move source and the handle after a
successful click are marked invalidated, and reusing them returns `StaleObservation`. After a
successful delivery it sets invalidated first and then sends the possibly-failing trace, avoiding a
trace failure that would induce a duplicate click.

**D0 authority and current state must be distinguished.**
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` requires the injection layer to re-check
both `FrameId` and `TargetGeneration`. The current Controller's `checkPointerPreconditions` actually
re-checks `CaptureSessionId`, lease age, `TargetGeneration`, and the client point, but has no "current
`FrameId`" parameter; `FrameId` is currently constrained by annotation/engine's same-frame comparison
and the single consumption of `Observation`. A maintainer should not treat the comment's "delivery
layer re-runs frameId fence" as already independently implemented by the Controller.

**Determinism first, time only as a fuse.** Identity equality is the primary criterion; the pure
coordinate transforms, integer bounds, and recovery mappings give the same output for the same input.
Monotonic time is used only for `max_action_frame_age`, to handle the escape case where the game
itself changes the interface but the generation has not changed; a timeout only influences the result
toward rejection.

**Ownership and lifetime are visible.** `Frame` shares the large pixel data via
`shared_ptr<const FrameBuffer>`, and the span of `bytes()` explicitly depends on the `FrameBuffer`
owner. `Detection`, lease, transform, and IDs are all carried by value. `domain` does not store a raw
pointer, callback, or asynchronous borrow.

**Strict-background is a cross-module invariant.** `domain` only provides `Point<ClientSpace>`, the
lease, and error semantics; `engine::IActionSink` requires the adapter to pass the lease down to the
delivery layer, and the Controller ultimately uses `PostMessageW`. `SetForegroundWindow`, `SetFocus`,
`SendInput`, `mouse_event`, `keybd_event`, and `SetCursorPos` are listed as forbidden. No domain
extension may degrade to foreground or global input on the grounds that "the coordinates are not
expressive enough".

## Consumers

The typical data flow is as follows:

1. `controller` resolves the window and client geometry, maintains `CaptureSessionId`/`TargetGeneration`,
   captures pixels, and allocates a `FrameId` per frame.
2. `CoordinateTransform::create` fixes the desktop client origin, client extent, and frame extent
   into the same `Frame`.
3. `Frame::create` validates the buffer geometry, transform size, and immutable pixel owner.
4. `engine::EngineSession::observe` first revalidates the target, then captures, and creates an
   `ObservationLease` from the frame.
5. `annotation::RecognitionRuntime`/`vision` produce integer `PixelRect` evidence on the frame
   pixels; engine creates a same-frame `Detection` via `pixelRectToFrameRect`.
6. annotation binds the detection to the `action_target` recognizer and proves that the page,
   project, fingerprint, identity triple, and lease are consistent.
7. engine converts the integer click pixel into a `Point<ClientSpace>` via `pixelPointToFramePoint`
   and the frame's `frameToClient`.
8. `engine::IActionSink::click` hands the client point together with the original lease to the
   Controller; the Controller re-checks the target generation, session, age, window liveness, and
   client bounds, then delivers it in the background via `PostMessageW`.
9. After success, engine invalidates the `Observation`, and the next action must observe again.

The inbound edge is primarily `core`: `Result`/`Status`, checked arithmetic/cast,
`StrongId`/`Generation`, enum reflection, `MonotonicInstant`, and release-safe contracts. `domain`
composes these general mechanisms into automation semantics, but does not push UmbraFlow policy down
into `core`.

Outbound consumers each take a portion rather than depending on an aggregate header:

- `vision` consumes `Frame`, `PixelRect`, and error types, and outputs deterministic matching
  evidence.
- `image` consumes pixel geometry and frame layout, and performs cropping and format conversion.
- `annotation` consumes frame identity, detection, lease, and geometry, and establishes page/action
  capability.
- `engine` organizes the observe/resolve/find/act lifecycle and lets trace carry domain identity and
  error kind.
- `controller` produces the frame/transform, maintains the target generation, and consumes the
  client point and lease.
- `script` ultimately sees these semantics through the host bindings, without directly obtaining the
  pixel owner or platform handles.

## Tests

`tests/domain/test-space.cpp` is the main test for the coordinate contract: known mappings,
round-trip tolerance, half-open containment, finite/bounds checks, `floor`/`ceil` coverage, subpixel
at least one pixel, `PixelRect` overflow, hash, and the `[0, 2^24]` inclusive boundary and integer
round-trip are all pinned here.

`tests/domain/test-frame.cpp` pins immutable shared pixels, `PixelFormat` byte counts, the minimum
legal values of stride/length, padding, zero sizes, multiplication overflow, an empty owner, and
transform/frame size consistency.

`tests/domain/test-ids.cpp` pins that the ID types cannot be mixed, that `TargetGeneration` is
monotonic and does not wrap, and the UTF-8 accept/reject behavior of `Label`.

`tests/domain/test-detection.cpp` pins that 750 ms can only be shortened, the strict boundary of the
deadline, per-item rejection of the identity triple, negative age, and deadline overflow. You must
understand these precise assertions before modifying the lease comparison order or the expiry
arithmetic.

`tests/domain/test-error.cpp` enumerates every `AutomationErrorKind`, pinning the detail name and
`FailureResponse`; it also pins the fail-closed behavior of a generic error and the bit-pattern
fidelity of high-bit OS codes such as `0x8007'0005`.

`tests/domain/test-time.cpp` pins monotonic add, saturation of reversed elapsed to zero, and the
rejection of negative duration and overflow.

Cross-module tests pin "how domain values actually land":

- `tests/controller/test-capture-wgc.cpp` pins `FrameId` monotonicity within a session and overflow
  rejection, and verifies that the capture geometry creates a transform.
- `tests/controller/test-target.cpp` pins which kind of window identity change advances
  `TargetGeneration` exactly once.
- `tests/controller/test-input-revalidation.cpp` pins session/generation/age fencing, client bounds,
  and the signed-16-bit message coordinate limit; it also reflects that there is currently no current
  `FrameId` parameter.
- `tests/controller/test-audit-log.cpp` pins the strict-background forbidden API set and the
  delivery audit.
- `tests/annotation/test-authorization.cpp` pins same-frame page/detection/lease/fingerprint,
  recognizer identity, and allowed-page authorization.
- `tests/engine/test-session.cpp` pins the observe-to-click data flow, verbatim lease forwarding,
  post-action invalidation, moved-from/foreign observation rejection, delivery-edge target
  revalidation, and that a trace failure cannot make an action replayable.

Most of these tests use synthetic frames and an explicit `MonotonicInstant`, avoiding real windows,
wall-clock jitter, or GPU timing entering domain's deterministic regression surface.

## Future Extensions

**P1 resolution adaptation.** `docs/plans/2026-07-21-product-form-and-roadmap.md` specifies that P0
uses the project `base_resolution`/DPI fingerprint and an identity gate, and only P1 adds an explicit
Base→Live viewport transform. It should be attached before or after `CoordinateTransform` as a new,
clearly named space or transform layer, and must not quietly change the meaning of the existing
`FrameSpace`, `ClientSpace`, or the `[0, 2^24]` bridge.

**Luau surface.** `docs/plans/2026-07-23-engine-architecture.md` requires that a future Luau mirror
`observe`/`resolvePage`/`findAction`/`act` 1:1. Domain values should continue to serve as the payload
of read-only host capabilities; do not expose a forgeable lease, a mutable frame identity, or bare
pixel lifetime for scripting convenience.

**Resident Engine and a second platform.** The same plan leaves engine ports for the P2 resident
lifecycle and a future non-Windows adapter. A new capture adapter must produce the same `Frame`, host
monotonic arrival time, increasing `FrameId`, target generation, and transform proof; a new action
adapter must preserve lease forwarding and strict-background, and must not leak platform differences
into domain.

**D0 delivery-layer reinforcement.** Per
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md`, the most clearly unclosed seam is to give
the Controller delivery a comparable current `FrameId`, so that it independently verifies the lease's
dual counters at the final delivery point rather than relying only on engine's single consumption.
Extending this requires adjusting both the `IActionSink`/Controller delivery contract and the
corresponding tests together, not just changing a comment.

**New error kinds.** When adding an item to `AutomationErrorKind`, you should keep the enum
reflection, the exhaustive `failureResponse` mapping, the trace/script boundary, and the complete
case table of `tests/domain/test-error.cpp` in sync. Whether to retry is a control-flow policy and
must not be guessed ad hoc from the error name.

**New pixel formats or coordinate spaces.** A new `PixelFormat` must define bytes-per-pixel, frame
geometry, vision/image support, and tests; a new coordinate space must be added to `CoordinateSpace`
and provide explicit conversions. Do not use "also a `float`" or "also a `uint32`" as a reason to
skip the space type.

**Action-kind extension.** The D1 authority treats any coordinate action as an "observe-then-single-
action". Future swipe, long press, or other pointer actions should reuse the same identity triple,
lease fuse, client conversion, and post-action invalidation semantics; keyboard actions currently
carry only `TargetGeneration`, and if they are brought into the same observe/act model, their
`FrameId` relationship must first be made explicit in the authority contract and the delivery API.
