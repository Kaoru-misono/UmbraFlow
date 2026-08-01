# Capture and target selection

Real-machine failure knowledge for `umbra-flow run` target discovery, WGC
capture, and the observation lease. First recorded during the B1 smoke
acceptance against 卡厄思梦境 (ChaosZeroNightmare via `ssr-stove-shield.exe`)
on 2026-07-25.

## Anti-automation decoy windows defeat a title-substring selector

### Symptom

`umbra-flow run --selector <game-title>` fails with
`selector "..." matches 25 windows; refine it: "...", "..._thread_0", ...`.
The refine list is dozens of near-identical titles: the real title plus many
`<title>_thread_0` and one `<title>_thread_<random>` entries, all from the same
PID.

### Root cause

The game's protection layer creates dozens of **invisible** top-level windows
whose titles are the real window title with a suffix, evidently to break
title-based window finding. `selectCandidate` matched on title substring alone,
so every decoy matched and the single-match requirement could never be met. The
decoys carry `IsWindowVisible == FALSE`; only the real game window is visible.

### Fix

`entry/cli/run-windows.cpp` `selectCandidate` now requires
`candidate.isVisible()` before a title match counts. The decoys drop out and the
single visible window resolves. Error text became "no visible window title
contains ...".

### Regression check

`ctest -L CI` (the selection helper has no unit seam yet — it is in an
anonymous namespace in the Windows-only entry). Real-machine check: with the
game running, `umbra-flow run --selector <title>` resolves exactly one target
instead of reporting an N-window ambiguity.

## "No visible window title contains X" usually means the target is minimized

### Symptom

`umbra-flow run` (or the workbench Capture) fails with `no visible window title
contains "<selector>"` even though the game is clearly running.

### Root cause

Target selection requires a **visible, non-minimized** window (`isVisible() &&
!isIconic()`), which is correct — a minimized window renders nothing to capture.
When the game is minimized every candidate is filtered out. This is easy to
misread as a DPI or elevation bug. Two traps make it confusing:

- `IsWindowVisible` returns TRUE for a minimized window (WS_VISIBLE is set); only
  `IsIconic` distinguishes it, so a minimized window is excluded by the
  `!isIconic()` clause, not the visibility clause.
- `Get-Process(...).MainWindowHandle` can point at a *minimized decoy* while the
  real rendered window is a different HWND, so a quick probe of that one handle
  reports `iconic=True` while the actual game window is fine. Enumerate all
  top-level windows and check each, rather than trusting `MainWindowHandle`.

### Fix

Not a code bug — restore the target window. A Medium-integrity helper cannot
restore a window owned by an elevated (High-integrity) game via `ShowWindow`
(UIPI); restore it from the taskbar or an elevated context.

### Regression check

Real-machine: with the game minimized, capture/run reports the "no visible
window" error; restoring it lets the same command resolve the target.

## `CaptureStalled` used to name the symptom and withhold "the window is minimized"

### Symptom

During a live annotation session on 2026-07-31, every capture and every click
against the game window failed with nothing but:

```
CaptureStalled: no new frame arrived
```

The operator spent real time suspecting the capture backend, the frame lease,
and the target handle in turn. The actual cause was that the game window had
been **minimized** — `IsIconic(hwnd)` was true.

### Root cause

Two things, and only the second is a code fault.

A minimized window composites nothing, so a stall is its *expected consequence*,
not a fault at all. This differs from the discovery-side symptom above: there,
minimization is caught before a session exists and reported as "no visible
window title contains X". Here the session was created while the window was up
and the window was minimized afterwards, so discovery's filter never runs again
and the only thing that fires is the stall fuse.

The code fault: `StallTracker::check` composed its message from the timeout
alone. The one fact that explains the stall — a fact one Win32 call away, and
one the operator can act on in a second — was never queried, so the message
named the symptom and withheld the cause. Note that the adjacent `itemClosed`
path already explains itself ("capture item was closed; rebuild the session"),
which is why a *destroyed* window usually reads better than a minimized one did.

### Fix

`StallTracker::check` now requires a `TargetWindowState` (`Composing`,
`Minimized`, `Destroyed`) beside the instant, so a stall cannot be reported
without an observation of the window. `observeTargetWindow` in
`windows-capture.cpp` supplies it from `IsWindow` then `IsIconic`, and
`stalledFrameFailure` (`modules/controller/source/controller/capture-stall.cpp`)
turns it into a message that names the state *and* the action:

> no new frame arrived within 1000 monotonic clock ticks: the target window is
> minimized, and a minimized window composites no frames at all, so a stall is
> the expected result rather than a capture fault. Restore the window from the
> taskbar and run again; a target running elevated has to be restored from an
> elevated context

The `Composing` message says the window state does *not* explain the stall,
rather than hinting at minimization when nothing was observed — a message that
cries "minimized" at every stall teaches the operator to ignore it.

Occlusion and an off-screen position are deliberately **not** probed. DWM keeps
composing for both, so neither can cause a stall, and reporting them would send
the next investigation in the wrong direction.

### Regression check

`ctest -L CI` (`test-controller`): the message tests are falsifiable — collapsing
the three explanations to one turns the minimized and destroyed cases red, and
making `StallTracker::check` ignore its `TargetWindowState` argument turns "the
tracker reports the window state it was given" red. The Win32 probe itself is
not covered: inverting `IsIconic(window) != FALSE` leaves all 95 controller
tests green, so that one predicate needs a real-machine check — minimize the
game mid-run and confirm the stall message says so.

## WGC capture bind needs the same integrity level as the target

### Symptom

`CaptureUnavailable: failed to bind capture session to the target window
instance (Win32 error 5)` (`windows-capture.cpp`, `SetPropW` site). Discovery
and recognition are fine; only capture binding fails. Error 5 is
`ERROR_ACCESS_DENIED`.

### Root cause

`WindowInstanceMarker` calls `SetPropW` on the target HWND to stamp a
capture-session identity token. `SetPropW` against a window owned by a
higher-integrity process is blocked by UIPI when the caller runs unelevated.
The game ran elevated / at higher integrity, so an unelevated `umbra-flow` (or
`m0-demo capture`) could not stamp the property. Note `PostMessage(WM_NULL)` to
the same window *did* succeed unelevated — input delivery and capture binding
have different integrity requirements, so a passing input probe does not imply
capture will bind.

### Fix

Run the capture-side binary at the target's integrity level (elevated when the
game is elevated). This matches the engine-architecture plan's Phase 3
single-process elevation assumption. No code change; it is an operating
constraint of WGC window-instance binding.

### Regression check

Real-machine: an elevated `m0-demo capture --hwnd <h> --out x.png` succeeds
where the unelevated one returns Win32 error 5.

## Debug-build recognition latency exceeds the 750 ms action-frame lease

### Symptom

A **debug-built** `umbra-flow run` reaches `ActionFound` with `sadScore:0`
(perfect match) and then `ActionRejected` / `StaleObservation: lease expired:
observation older than max action frame age`, delivering zero clicks. Raising
`--max-frame-age` does not help (it is clamped to 750 ms).

### Root cause

Recognition itself is slower than the lease in a debug build. The action-frame
lease fences `now - frame.capturedAt <= 750 ms` at `act()`
(`k_defaultMaxActionFrameAge`, `domain/detection.cpp`; `--max-frame-age` is
clamped to it). The SAD template match is a pure scalar triple loop with
per-pixel bounds-checked span access and a per-pixel 64-bit modulo, with no
SIMD (`vision/sad.cpp`). Under `/Od /RTC1` that is ~30x slower than release.

Measured on a real 1600x900 capture with the smoke project's ROIs
(`evaluatePage` over a 340x100 ROI + `evaluateActionTarget` over a 200x90 ROI),
via a throwaway timing probe calling `RecognitionRuntime` directly:

| build | evaluatePage | evaluateActionTarget | total |
|-------|--------------|----------------------|-------|
| x64-debug   | ~677 ms | ~355 ms | **~1030 ms** |
| x64-release | ~21 ms  | ~11 ms  | **~32 ms**   |

So in debug, recognition alone (~1030 ms) exceeds the 750 ms lease before the
click is even attempted — with a perfectly fresh frame. This has nothing to do
with frame rate. An earlier "~2 fps background rendering" theory was wrong: that
number came from `m0-demo capture --frames 30`, whose `capture_fps` is a
cumulative wall-clock rate that includes each frame's PNG encode + disk write +
inter-frame sleep (`entry/m0-demo/capture-mode.cpp`), not the game's render
rate.

### Fix

No code change. The 750 ms lease is a release-tuned budget; run the real-machine
smoke and any production automation with a **release** build. Release recognition
(~32 ms) leaves ample headroom under the lease. The lease and the WGC capture
path (including the separate `CaptureStalled` timeout for a genuinely stalled,
occluded window) are correct as designed — do not loosen the clamp, and no
`capture()` freshness change is warranted.

### Regression check

The Fake `FrameSource` fail-closed suite pins that an expired lease yields zero
delivery (`ctest -L CI`). To reproduce the latency gap, time
`RecognitionRuntime::evaluatePage`/`evaluateActionTarget` on a 1600x900 frame in
debug vs release; debug is ~1 s, release is tens of ms.

## CJK selectors work; a mojibake selector is a caller-side encoding mistake

### Symptom

`umbra-flow run --selector 卡厄思梦境` appears to fail with
`no visible window title contains "鍗″巹"` — the selector shows up as mojibake.

### Root cause

Not a product bug. The `umbra-flow` / `m0-demo` executables embed a UTF-8
`activeCodePage` manifest (`cpp_apply_utf8_manifest` in `entry/CMakeLists.txt`),
so the process ANSI code page is UTF-8 and `main(int, char**)` receives argv as
UTF-8 — matching the UTF-8 window titles discovery produces
(`utf16BufferToString`). A literal CJK `--selector` on the command line matches
correctly. The mojibake above was self-inflicted: the caller passed a string
that had already been re-decoded through GBK (`鍗″巹` is `卡厄` reinterpreted as
GBK), so the tool faithfully searched for the wrong string.

### Fix

None needed. Pass the intended UTF-8 characters. When scripting the invocation,
make sure the launcher hands the child real argument strings rather than bytes
that were round-tripped through a legacy code page.

### Regression check

Real-machine: `umbra-flow run --selector <CJK-title>` with the correct
characters resolves the target; there is no encoding step in the tool to fix.

## `--pid` alone cannot select this game's window

### Symptom

`m0-demo capture --pid N` fails with `TargetUnavailable: 103 windows matched
selector pid=N; disambiguate with --pid or --hwnd`, and the advice is unhelpful
because `--pid` is what was already passed.

### Root cause

The same anti-automation decoys that defeat a title-substring selector also
defeat a bare pid selector: one process owns 103 top-level windows, of which one
is the real `GLFW30` window titled 卡厄思梦境 and the rest are `_thread_N`
clones, `NVOpenGLPbuffer` surfaces, and `IME` windows. The visible-window filter
that fixed the title selector (`entry/cli/run-windows.cpp`) is not on this path.

### Fix

Pass `--hwnd` with the real window's handle. It is the entry in the refine list
whose class is `GLFW30` and whose title is exactly the game's, with no
`_thread_N` suffix.

### Regression check

Real-machine: `--pid` alone still reports the ambiguity; `--hwnd 0x…` on the
`GLFW30` entry captures.


## A hand-rolled click sequence wakes the HUD but presses nothing; use the controller's

### Symptom

Posting `WM_MOUSEMOVE`, `WM_LBUTTONDOWN`, `WM_LBUTTONUP` to the game window by
hand reliably makes the hidden HUD appear and then does nothing else. No menu
entry activates, no page changes, and nothing reports an error at any layer.

Because the HUD wakes, it looks like proof the click was received and acted on —
which sends every subsequent guess in the wrong direction. Four hypotheses were
tested and eliminated this way on 2026-07-30 (settle timing, target position,
physical cursor position, foreground focus) before the actual cause surfaced.

### Root cause

The hand-rolled sequence held the button down for 60–100 ms between DOWN and UP.
`controller::click` holds it for **zero** — `deliverPointerDown` then
`deliverPointerUp` with nothing between them (`modules/controller/source/controller/input.cpp`).
This game evidently reads a held button as a drag rather than a tap.

The lesson generalises past the timing: the controller's delivery path is the
tested one, and reproducing it approximately is not the same as using it. It also
validates an observation lease, revalidates the target instance, and audits the
posted messages — none of which a hand-rolled `PostMessageW` does.

### Fix

Drive input through `umbra-input-agent`, which is the same delivery path the
product uses:

```
umbra-input-agent --hwnd 0xHWND --queue q.jsonl --results r.jsonl --output-dir DIR
```

> Renamed 2026-07-31. This was `m0-demo input-agent` until the annotation
> front-end moved out of the frozen demo into `entry/input-agent` and got its
> own binary. The old spelling now prints where the program went and exits with
> failure, so a stale script fails loudly rather than parsing as demo arguments.

then append one JSON line per request. Coordinates are client pixels:

```json
{"op":"click","x":1447,"y":247,"out_before":"b.png","out_after":"a.png","settle_ms":1500}
{"op":"capture","out":"frame.png"}
{"op":"quit"}
```

The agent captures before and after each click, so the outcome is verifiable from
the frames rather than inferred. Two operational notes: the queue and results
files must live **outside** `--output-dir`, and the agent is long-running, so
launch it detached (`Start-Process`) — a PowerShell `Start-Job` dies with the
session that created it.

### Regression check

Real-machine: queue a click on a menu entry, then match that page's anchor
against `out_before` and `out_after`. The anchor hits before and misses after,
which is the page having changed. A bare hand-rolled sequence with a hold
between DOWN and UP leaves the anchor hitting both.

## A target's own behaviour does not belong in this file

This file records toolchain pitfalls: window selection, capture, delivery,
build. How a **particular game** behaves — how long until it hides its HUD, how
long its UI takes to fade in, which rectangles never hold still — belongs to
that project and is written in the project's own directory, for example
`E:\umbraflow-projects\chaos-daily\PITFALLS.md`.

This repository is a reusable foundation. Knowledge that stops being true when
the target changes costs every future reader context it cannot repay.

A section whose symptom involves one game but whose lesson is about our own CLI
or controller — a pid mapping to several windows, a hand-rolled click path
being the wrong path — stays here.
