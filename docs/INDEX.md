# Documentation Index

## Start here

- [Runtime v2 and game-operator breaking authority](plans/2026-08-09-runtime-hardening-rewrite.md)
- [Requirement and migration map](plans/2026-08-09-runtime-migration-report.md)
- [Architecture](ARCHITECTURE.md)
- [Current execution checklist](TODO.md)
- [Independent review outcome, 2026-08-10](reviews/2026-08-10-runtime-hardening-review.md)
  — both reviews returned FAIL; the open findings are the delivery blockers.
- [Historical pre-rewrite work queue](WORKLIST.md) — retained evidence only,
  not an implementation queue.

## Current plans

The first six below are the reading order; the dated decisions after them build on
it. [`plans/README.md`](plans/README.md) carries each plan's status and is the
canonical listing — this section is its short form. Everything else is archived.

- Target form — three layers plus the Agent operator (approved 2026-08-01):
  [Three layers and the Agent operator](plans/2026-08-01-three-layers-and-agent-operator.md)
- Layer ownership — element and page move up to trusted Luau (ruled 2026-07-31,
  reconciled 2026-08-01 in §十二); the migration now in progress:
  [Script-owned page model](plans/2026-07-31-script-owned-page-model.md)
- Current product direction:
  [Product form and Roadmap](plans/2026-07-21-product-form-and-roadmap.md)
- Current task-system architecture (approved 2026-07-29; its layer-one boundary
  is amended by the script-owned page model above):
  [Three-layer task system](plans/2026-07-29-three-layer-task-system.md)
- Current annotation-model decisions (approved 2026-07-31, slimmed 2026-08-01 to
  §二 design and §四之二 rulings; design conclusions stand, implementation
  position moved by the script-owned page model above):
  [Annotation model — capabilities, holding, appearances](plans/2026-07-31-annotation-model-capabilities.md)
- Implementation shape of the exploration environment (authorised 2026-08-01):
  [Agent front end and the exploration environment](plans/2026-08-01-agent-front-end-and-exploration.md)
- State layer and policy slots — `l2-v2`, five rulings answered 2026-08-04, four
  phases of which only A has landed:
  [State layer and policy slots](plans/2026-08-04-state-layer-and-policy-slots.md)
- Keeping the screenshot corpus out of version control — tier 4 shipped
  2026-08-04, tier 0 retired by measurement, tiers 1–3 still proposals:
  [Storing the evidence corpus](plans/2026-08-04-evidence-storage.md)
- Framework capabilities for full-map route planning (settled 2026-08-05, pending
  execution) — the drag and connectivity verbs, and three things ruled out:
  [Framework capabilities for full-map planning](plans/2026-08-05-map-verbs-and-connectivity.md)
- Proposal awaiting a decision, no code changed on its account:
  [Luau coding standard — measurements and outline](plans/2026-08-02-luau-coding-standard.md)
- Frozen real-machine acceptance ledger, retained until parity retires it:
  [M0 demo port deviations](plans/2026-07-20-m0-demo-port-deviations.md)
- [Plans](plans/README.md)

## Archive

Archived planning and research material lives under `archive/plans/`; closed
reviews live under `archive/reviews/`. `docs/reviews/` was empty between
2026-08-01, when its three reviews were closed and moved, and 2026-08-10, when
the runtime-hardening review reopened it. Archive that one too once its open
findings are closed.

- [Locked S0 annotation contract](archive/plans/2026-07-22-annotation-design.md)
- [Luau task-model grill decisions](archive/plans/2026-07-21-lua-task-model-grill-decisions.md)
- [P0-B script layer](archive/plans/2026-07-27-p0b-script-layer.md)
- [Lua task-model decision package](archive/plans/2026-07-21-lua-task-model-decision-package.md)
- [Safe C++ core plan](archive/plans/2026-07-20-safe-cpp-core.md)
- [Luau-first task system design draft](archive/plans/2026-07-28-luau-first-task-system-design-draft.md)
  — superseded in full on 2026-07-29 by the three-layer task system.
- [Annotation backend branch review](archive/reviews/2026-07-22-annotation-backend-review.md)
- [Luau-first draft review](archive/reviews/2026-07-28-luau-first-draft-review.md)
  — the review whose conclusions the three-layer plan adopted.
- [Full-project architecture review](archive/reviews/2026-07-27-full-project-review.md)
- [Repo-wide C++ simplification sweep](archive/reviews/2026-07-25-simplify-sweep.md)

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
