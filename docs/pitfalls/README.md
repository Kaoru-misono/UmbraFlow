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
- [Operator schema identity is the STORED DDL text](operator-schema-identity-and-stored-ddl.md)
  — two schemas with identical columns hash differently because SQLite stored
  the trailing whitespace one of them was executed with.
- [A framework constant a durable row pins](framework-constants-pinned-in-durable-rows.md)
  — why a format cut refused every Operator root that existed before it, and the
  registered-predecessor migration that keeps generation 0 layout rather than
  history.
- [The retired observation body and ledger call tree](observation-frame-and-ledger-children.md)
  — historical nested-call failures; the transferable rules cover independently
  admitted relationships, resource-scope ownership, independent interactive
  roots, and producer-shaped fixtures.
- [Ad-hoc probe binaries put modal dialogs on the screen](ad-hoc-probe-binaries.md)
  — a hand-built executable links none of the crash-box suppression the test
  main does, so an access violation blocks on a dialog nobody sees in a log;
  read a historical identity out of git rather than compiling something to
  print it.
- [Concurrent agent builds in one worktree](concurrent-agent-builds.md)
- [Running the repository's own tooling](repository-tooling-invocation.md) — a
  repo-wide formatter rewrites files another agent owns, the documented MSVC
  activation command runs nothing when invoked from the Bash tool, and an edit
  under `examples/` is invisible until CMake reconfigures. All three fail by
  looking like success.
- [Cross-platform CI toolchain diagnostics](cross-platform-ci-toolchains.md)
- [Capture and target selection](capture-and-target-selection.md) — which window
  of the hundred a game owns is the real one, why a hand-rolled click path wakes
  the HUD and presses nothing, and why a posted wheel scrolls somewhere else.
- [Page modeling and multi-step flows](page-modeling-and-multi-step.md)
- [Element choice and thresholds](element-choice-and-thresholds.md) — what to
  annotate and what number to give it; start here before drawing a rectangle.
- [Colour-key annotation](colour-key-annotation.md) — measured colour-key
  failures. Its invocation spellings are historical by design; the thresholds
  and failure shapes are not.
- [Luau patterns and long strings](luau-patterns-and-long-strings.md) — a
  regex-style optional group never matches, and TOML `[[section]]` closes a
  level-0 long string; both fail quietly.
- [Embedded VM memory ceilings](embedded-vm-memory-ceiling.md) — what a hard
  ceiling measures, why protection is per Luau state, and how to falsify a
  memory regression.
- [Trusted Framework module resolution](trusted-framework-module-resolution.md)
  — why exact reserved imports can work in PureDataProgram yet fail during a
  full Engine boot, and the ordering/isolation rules the two loaders must keep.
  Also why a projected framework name spelled like a standard-library global
  replaces that library for every project script.
