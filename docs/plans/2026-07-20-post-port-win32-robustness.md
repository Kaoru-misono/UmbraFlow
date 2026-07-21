# Post-port product-phase considerations: Win32 discovery/DPI robustness

Status: deferred to the product-redefinition phase (NOT port-stage work).
Source: controller 03a safety review panel, adjudicated 2026-07-20.

The safety panel found no C++ UB and no leaked handles in the controller
03a port. It did raise six Win32 robustness observations (S-1..S-6). Each
was checked against the Rust source and confirmed to be a FAITHFUL PORT of
the Rust behavior — the C++ matches Rust, including Rust's own `SAFETY`
comments and documented preconditions. They are therefore NOT port defects
and were intentionally left unchanged at the port stage: changing them
would make the C++ diverge from the frozen Rust reference.

They are recorded here because they are genuine design limitations the
Rust original also carries, worth revisiting when the product is
redefined (the phase where improving on the Rust design is in scope).

- **S-1 / S-2 — HWND recycling races.** Discovery and live-identity
  revalidation observe a window through several non-atomic Win32 reads
  keyed by an `HWND`, which the OS may recycle. A destroyed window whose
  handle is reused by an equally sized same-process window can be
  classified `Unchanged` (no generation bump), or a candidate can combine
  one window's process details with another's title/geometry. Rust:
  `discovery.rs:322` (IsWindow, with a SAFETY note acknowledging stale
  handles) and the non-atomic `probe_candidate`. Product-phase options:
  re-validate identity atomically where feasible, or add a
  post-observation identity re-check.
- **S-3 — GetWindowThreadProcessId failure becomes PID 0.** Rust
  initializes `pid = 0` and does not check the return, so a failed read
  yields a PID-zero candidate. Product-phase: treat failure as
  candidate-invalid.
- **S-4 — Title/class buffer truncation.** Fixed buffers (title 512,
  class 256 UTF-16 units) truncate silently; a long title matches on its
  truncated prefix. Identical in Rust (`[0u16; 512]` / `[0u16; 256]` +
  `from_utf16_lossy`). Product-phase: grow/query length or flag
  truncation to the selector.
- **S-5 — GetDpiForWindow == 0 stored as a valid Dpi.** The documented
  failure value 0 is passed through. Product-phase: reject 0 as
  invalid-DPI.
- **S-6 — Process-DPI verification via thread context.** `AlreadyDeclared`
  on ACCESS_DENIED is confirmed via the calling thread's DPI context,
  correct only under the documented precondition "call before any
  thread-level DPI override." Rust documents this precondition explicitly.
  Product-phase: query the process default directly if a stronger
  guarantee is wanted.

## Input chain (controller 03b), adjudicated 2026-07-20

The ADR-011 safety panel confirmed no code path reaches any forbidden
foreground/global-input API. These additional input-side items are
faithful ports of the Rust behavior (checked against the Rust source) and
are deferred to the product phase:

- **Up compensation posts to a stored, possibly-recycled HWND.**
  `releaseHeld`/`release_all` post best-effort Ups to the stored handle
  without live process-instance revalidation. Rust `held.rs::release_all`
  is identical; DESIGN §9.2 defines Up compensation as best-effort.
  Product-phase: optionally revalidate identity before each Up.
- **Held state is cleared before Up attempts.** On post failure the retry
  state is gone. Faithful to Rust (both clear-before-attempt). Product-
  phase: consider retry/persisted-held semantics if reliability demands.
- **DeliveryTarget carries no compatibility-verified proof.** End-to-end
  fail-closed on an unverified action type belongs to the capability/
  compatibility wave (DESIGN §6.2, `TargetCompatibilityUnverified`), not
  the input port.
- **MapVirtualKeyW zero (no translation) becomes scan code 0.** An
  unmapped key posts with an unverifiable encoding. Faithful to Rust.
  Product-phase: reject unmapped keys with `ActionRejected`.
- **check_safety.py static ADR-011 gate is a backstop, not a boundary.**
  It catches direct calls and address-taking, and correctly ignores the
  forbidden-name string list and comments, but a regex cannot catch
  `GetProcAddress("SetForegroundWindow")` or token-pasted names. We are
  the authors; this is an accepted limitation, not a defended boundary.

Taken NOW (not deferred), as ADR-011 hardening with no Rust-fidelity
impact: the static gate was extended with `BringWindowToTop`,
`SwitchToThisWindow`, `AttachThreadInput`, `SetActiveWindow`; the delivery
choke point now rejects NULL/`HWND_BROADCAST` fail-closed.

## WGC capture (controller 03c), adjudicated 2026-07-20

The capture safety panel found no direct C++ data race, torn read, or
callback use-after-free (the FrameSlot and frame pool are captured by
value; all newest-frame access is under the slot mutex). These items are
faithful ports of the Rust behavior (checked against the Rust source) and
are deferred to the product phase:

- **A late in-flight FrameArrived callback can repopulate the frame slot
  after teardown clears it**, retaining that frame until session
  destruction. Rust's `close` likewise only `RemoveFrameArrived` without
  draining in-flight callbacks, and neither implementation checks a
  closed flag inside the callback. The slot is shared-owned and the frame
  is released at session destruction — not UB, not a permanent leak.
  Product-phase: add a shared closed flag the callback checks before
  publishing.
- **`close()` cannot interrupt a `capture()` wait.** Both take the
  operation mutex and `close` does not notify the frame condvar, so a
  concurrent close blocks until the capture timeout elapses. Rust's close
  also does not notify the wait; irrelevant on the single-threaded demo
  consumer path. Product-phase: add a shutdown signal that notifies the
  waiter.

Taken NOW (not deferred), as safety hardening beyond the Rust reference
(Rust shares the latent issue) with no observable-behavior change:
`ID3D11Multithread::SetMultithreadProtected(TRUE)` is enabled on the
capture D3D11 device, because the immediate context is shared with the
free-threaded WGC frame-pool worker and an unprotected context is UB-class
(driver corruption) — worth closing before real-machine capture testing.

None of the deferred items block the M0 path: the demo runs single-window
against a known target, and the DPI helper runs once at startup before any
override.

## Real-machine finding: WGC frame-stall vs. lease staleness (2026-07-21)

Observed during the real-machine UI acceptance run against the live elevated
game (卡厄思梦境, hwnd 0x51180, client 1600×900, delta=0). The before/after
click verification PASSED (avatar switch ×3, tab switch ×3, first-time
"potential" intro overlay recognized and safely dismissed; every delivered
click went through the strict-background PostMessage chain with K2 crop
delta=0). Two behaviors surfaced that belong to the product phase:

- **Lease fail-closed fired correctly (not a defect).** Clicks whose
  before-observation aged past `max_action_frame_age` were rejected with
  `StaleObservation: lease expired` and `delivered:false` — the
  "never act on a stale observation" guarantee working on real hardware.
- **WGC stalls on static / subtly-animated screens, turning that guarantee
  into a FALSE rejection.** WGC delivers a new GPU frame only on visual
  change. On the 潛力 potential-tree page (which *looks* animated but did
  not trigger WGC's dirty-region capture) no fresh GPU frame arrived for
  >750ms, so the frame each click op captured carried a WGC
  `SystemRelativeTime` older than `max_action_frame_age`, and every click
  there fail-closed even though the screen had not actually changed. A
  standalone `capture` op still "succeeds" (its own frame-id counter
  advances 23→24), but the underlying GPU frame timestamp is old and the
  lease judges age by that timestamp — so capture works while click is
  refused. Auto-farming visits many such screens, so this blocks unattended
  runs, not just this test. Product-phase options (tie to D1/D2
  `max_action_frame_age` calibration in
  [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md)):
  request a genuinely NEW frame and wait for it before acting; distinguish
  "no new frame because static" from "capture stalled/occluded" (a static
  screen is safe to act on, a stalled capture is not); or grant known-static
  pages a longer staleness budget. Any fix MUST preserve the fail-closed
  guarantee for genuine staleness.
