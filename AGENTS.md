# AGENTS.md

All project instructions live in **`CLAUDE.md`** — read it first. It is the single
source of truth for build commands, verification, pitfalls, ownership, and workflow
rules, and it applies to every agent (Claude, Codex, …) working in this repo.

Codex note: your ephemeral plan scratch dir is `.Codex/plans/`; the permanent record
of any finalized plan still goes to `docs/plans/YYYY-MM-DD-<topic>.md`.

Codex git reminder: before staging or committing, inspect the recent repository
history (`git log --oneline -20`) and follow the existing commit-message style.
Do not invent ad hoc commit titles. Split commits by task semantics so each
commit has one reviewable purpose; architecture changes, runtime fixes, tests,
docs, and assets should be separate unless they are the same minimal behavior
change. Never turn "commit the workspace" into one catch-all commit. Use explicit
path staging, review the staged diff, and verify the final commit list before
reporting back.
