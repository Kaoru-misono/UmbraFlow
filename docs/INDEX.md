# Documentation Index

## Start here

- [Architecture](ARCHITECTURE.md)
- [Current execution checklist](TODO.md)

## Current plans

- Target form — three layers plus the Agent operator (approved 2026-08-01):
  [Three layers and the Agent operator](plans/2026-08-01-three-layers-and-agent-operator.md)
- Layer ownership — element and page move up to trusted Luau (ruled 2026-07-31,
  reconciled 2026-08-01 in §十二):
  [Script-owned page model](plans/2026-07-31-script-owned-page-model.md)
- Current product direction:
  [Product form and Roadmap](plans/2026-07-21-product-form-and-roadmap.md)
- Current task-system architecture (approved 2026-07-29; supersedes the
  script-layer rulings below):
  [Three-layer task system](plans/2026-07-29-three-layer-task-system.md)
- Current annotation-model decisions (approved 2026-07-31; design conclusions
  stand, implementation position moved by the script-owned page model above):
  [Annotation model — capabilities, holding, appearances](plans/2026-07-31-annotation-model-capabilities.md)
- Superseded in full on 2026-07-29 by the three-layer task system, retained as
  history:
  [Luau-first task system design draft](plans/2026-07-28-luau-first-task-system-design-draft.md)
- Review whose conclusions the three-layer plan adopted:
  [Luau-first draft review](reviews/2026-07-28-luau-first-draft-review.md)
- [Plans](plans/README.md)

## Archive

Archived planning and research material lives under `archive/plans/` (ten more
completed plans moved there on 2026-08-01); closed reviews live under
`archive/reviews/`.

- [Locked S0 annotation contract](archive/plans/2026-07-22-annotation-design.md)
- [Luau task-model grill decisions](archive/plans/2026-07-21-lua-task-model-grill-decisions.md)
- [P0-B script layer](archive/plans/2026-07-27-p0b-script-layer.md)
- [Lua task-model decision package](archive/plans/2026-07-21-lua-task-model-decision-package.md)
- [Safe C++ core plan](archive/plans/2026-07-20-safe-cpp-core.md)
- [Annotation backend branch review](archive/reviews/2026-07-22-annotation-backend-review.md)

The knowledge base (`docs/knowledge/`) was deleted on 2026-08-01: with the code
framework mid-migration it was pure maintenance burden. Reusable failure
knowledge stays in [Pitfalls](pitfalls/README.md).

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
