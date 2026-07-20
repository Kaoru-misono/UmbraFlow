# M0 demo port deviations

Status: accepted port deviations, adjudicated 2026-07-20.

These findings were reviewed against the Rust source in
`.reference/rust/examples/m0-demo/` and intentionally require no C++ code
change:

- **F-2 / F-3 / F-7 — error wording.** The logical error kinds match and the
  C++ messages are clear. Reproducing Rust standard-library parse text such as
  `invalid digit found in string` would over-fit the port to Rust implementation
  details.
- **F-4 — whitespace trimming.** Rust trims Unicode whitespace while C++ trims
  ASCII whitespace. The difference requires pathological command-line input.
- **F-5 — extreme float underflow.** Rust rounds `1e-100` to `0.0`, while the C++
  `from_chars` path rejects it. This is pathological input and both paths report
  `InvalidResource`.
- **F-6 — duration range.** C++ caps durations at the representable
  `steady_clock` range and fails closed. A `u64::MAX` millisecond duration is
  absurd and this follows the port's other duration adaptations.
- **F-11 — held-release detail text.** The detail string format differs, while
  the structured fields and record ordering match.
- **F-13 — template quotas.** The 8192-pixel axis limit and 64 MiB encoded-file
  limit are intentional hardening. A real template is never expected to reach
  either cap.
- **F-14 — PNG CRC validation.** stb does not validate PNG CRCs. Templates are
  trusted input, and a corrupt template would surface as recognition failure
  rather than a silent wrong action.
- **F-18 — interruptible-sleep remainder arithmetic.** The seeded delay sequence
  is identical. Only sub-tick stop-observation timing differs.
- **F-19 — Windows console events.** C++ handles `CTRL_C_EVENT` and
  `CTRL_BREAK_EVENT`; Rust's `ctrlc` integration also signals on close, logoff,
  and shutdown. Windows may hard-kill either implementation during those latter
  events, while ordinary Ctrl-C behavior matches.
- **F-20 — terminal ordering on redundant flush failure.** Success-summary and
  fatal ordering can differ only when the redundant outer flush alone fails.
  Both implementations exit with failure.
