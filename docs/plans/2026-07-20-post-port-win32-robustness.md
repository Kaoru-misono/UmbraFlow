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

None of the deferred items block the M0 path: the demo runs single-window
against a known target, and the DPI helper runs once at startup before any
override.
