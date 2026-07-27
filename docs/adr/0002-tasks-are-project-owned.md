# Tasks are project-owned and name-addressed, never loose-path scripts

A task script could simply be a file path handed to the CLI, which is the
cheapest P0 shape. We decided instead (2026-07-27) that a task always lives
inside its project (`tasks/` directory) and is addressed as
`(project, task name)` — `umbra-flow run --project <dir> --task daily`. The
host computes the script's content hash at load time and records it in the
trace alongside the compiler version.

**Why:** the final product form (P2 tray app) opens a project and lists its
tasks, and P1 cross-file reuse (D7) requires manifest-declared,
content-hash-addressed sources. Both build on project ownership; a loose-path
CLI would force migrating every workflow later. The load-bearing commitment is
the addressing model — P0 uses a directory convention (task name = file stem),
and the P1 task manifest extends it additively once D7's real requirements are
known, rather than designing that format blind today.

**Consequences:** the CLI never executes a script from an arbitrary path;
trace reproducibility (veto #4) gets its script-hash input from day one; the
P1 task manifest and P2 task enumeration are additions, not migrations.
