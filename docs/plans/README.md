# Plans

Active and recent implementation plans live here. This is the canonical plan
location required by `CLAUDE.md` and `AGENTS.md`.

## Current authorities

- [Product form and Roadmap](2026-07-21-product-form-and-roadmap.md) — product direction,
  P0–P3 scope and exit criteria.
- [Luau task-model grill decisions](2026-07-21-lua-task-model-grill-decisions.md) — implementation-level
  decisions to revalidate when entering each phase.
- [Task-model decision package](2026-07-21-lua-task-model-decision-package.md) — research and arguments;
  not authoritative when it conflicts with the two documents above.

Active implementation plans (not authorities, executed when entering the phase):

- [P0-B Luau hardening ledger](2026-07-21-p0b-luau-hardening-ledger.md) — implementation-time
  checklist from two independent reviews of the Luau decision (cancel yield-abandon, deep-freeze,
  determinism gaps); apply during P0-B.
- [UI verification runbook](2026-07-20-ui-verification-runbook.md) — real-machine before/after
  click acceptance procedure.

The 2026-07-20 Lua grill is retained as historical input. It must not be used
as the current language or Roadmap decision.

A plan should be self-contained: it should include enough research, file paths,
implementation steps, and verification commands for another agent to execute it
without rediscovering context.

When a plan is completed and verified, archive it with the archive workflow
rather than leaving completed work mixed into active planning.
