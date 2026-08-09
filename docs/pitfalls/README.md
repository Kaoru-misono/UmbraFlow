# Pitfalls

Record reusable, verified failure knowledge here. Organize entries by subsystem
or module rather than by incident date.

Each entry should contain the symptom, root cause, fix, and a regression check. Do not use this directory for speculative notes.

## Entries

- [Concurrent agent builds in one worktree](concurrent-agent-builds.md)
- [Cross-platform CI toolchain diagnostics](cross-platform-ci-toolchains.md)
- [Capture and target selection](capture-and-target-selection.md)
- [Page modeling and multi-step flows](page-modeling-and-multi-step.md)
- [Element choice and thresholds](element-choice-and-thresholds.md) — what to
  annotate and what number to give it; start here before drawing a rectangle.
- [Colour-key annotation](colour-key-annotation.md)
- [Luau patterns and long strings](luau-patterns-and-long-strings.md) — a
  regex-style optional group never matches, and TOML `[[section]]` closes a
  level-0 long string; both fail quietly.
- [Embedded VM memory ceilings](embedded-vm-memory-ceiling.md) — what a hard
  ceiling measures when the runtime has no emergency GC, and the two shapes of
  memory test that prove nothing.
- [Workbench authoring UI](workbench-authoring-ui.md) — **historical since
  2026-07-31**: the GUI it documents was archived, so every prescription is
  unreachable. Its banner lists the rules that still transfer.
