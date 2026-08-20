# Pitfalls

Record reusable, verified failure knowledge here. Organize entries by subsystem
or module rather than by incident date.

Each entry should contain the symptom, root cause, fix, and a regression check. Do not use this directory for speculative notes.

## Entries

- [Checks that cannot fail](checks-that-cannot-fail.md) — a taxonomy of the
  false-green shapes, the detector appropriate to each one, and the
  falsification rule that a red must land at the assertion naming the property.
- [Project Kit is not production project admission](project-kit-is-not-production-admission.md)
  — why `project build/check/freeze/run` can be green while production open
  rejects a cross-document schema or RuntimeArtifact inconsistency.
- [Concurrent agent builds in one worktree](concurrent-agent-builds.md)
- [Running the repository's own tooling](repository-tooling-invocation.md) — a
  repo-wide formatter rewrites files another agent owns, and the documented
  MSVC activation command runs nothing when invoked from the Bash tool.
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
  ceiling measures, why protection is per Luau state, and how to falsify a
  memory regression.
