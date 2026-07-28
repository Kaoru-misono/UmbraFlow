# Documentation Index

## Start here

- [Architecture](ARCHITECTURE.md)
- [Current execution checklist](TODO.md)
- [System architecture overview](knowledge/en/00-overview.md)

## Knowledge base

Mirrored English and Chinese trees; see the [language guide](knowledge/README.md).

- [Knowledge base reading guide (English)](knowledge/en/README.md) — ordered navigation
  through the reusable modules and executable entry points.
- [架构知识库导读（中文）](knowledge/cn/README.md)

## Current plans

- Current product direction:
  [Product form and Roadmap](plans/2026-07-21-product-form-and-roadmap.md)
- Current task-system architecture (approved 2026-07-29; supersedes the
  script-layer rulings below):
  [Three-layer task system](plans/2026-07-29-three-layer-task-system.md)
- Current task-model decisions:
  [Luau task-model grill decisions](plans/2026-07-21-lua-task-model-grill-decisions.md)
- Locked S0 annotation contract:
  [P0-A Visual Annotation System & Data Model](plans/2026-07-22-annotation-design.md)
- P0-B script-layer decisions and slicing (rulings and API sketch superseded
  2026-07-29; retained as the record of landed stage work):
  [P0-B script layer](plans/2026-07-27-p0b-script-layer.md)
- Superseded in full on 2026-07-29 by the three-layer task system, retained as
  history:
  [Luau-first task system design draft](plans/2026-07-28-luau-first-task-system-design-draft.md)
- Review whose conclusions the three-layer plan adopted:
  [Luau-first draft review](reviews/2026-07-28-luau-first-draft-review.md)
- Completed full-project review follow-up:
  [Review fix record](plans/2026-07-28-full-project-review-fixes.md)
- [Plans](plans/README.md)

## Archive

Archived planning and research material lives under `archive/plans/`:

- [Lua task-model decision package](archive/plans/2026-07-21-lua-task-model-decision-package.md)
- [Lua task-model decision package — full](archive/plans/2026-07-21-lua-task-model-decision-package.FULL.md)
- [UI verification runbook](archive/plans/2026-07-20-ui-verification-runbook.md)
- [Safe C++ core plan](archive/plans/2026-07-20-safe-cpp-core.md)
- [Lua task-model grill](archive/plans/2026-07-20-lua-task-model-grill.md)

Closed reviews live under `archive/reviews/`:

- [Annotation backend branch review](archive/reviews/2026-07-22-annotation-backend-review.md)

## Repository guidance

- [Domain glossary](../CONTEXT.md)
- Architecture decision records: the two ADRs under `adr/` were deleted on
  2026-07-29 and the directory is empty. Their reasoning is preserved in
  [Three-layer task system](plans/2026-07-29-three-layer-task-system.md) — script
  handles as in-process userdata in §11, project-owned name-addressed tasks
  in §6. Decisions now land in dated plans under `plans/`.
- [Pitfalls](pitfalls/README.md)
- [C++ coding skill](../.claude/skills/cpp-coding/SKILL.md)
- [Safe C++ profile](../.claude/skills/cpp-coding/references/safety-profile.md)
- [Core capability evaluation skill](../.claude/skills/evaluate-core-capability/SKILL.md)
- [Git change management skill](../.claude/skills/manage-git-changes/SKILL.md)
