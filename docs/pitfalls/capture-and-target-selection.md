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
