# Documentation Index

## Start here

Three documents carry the current authority. The rewrite is the design, the next
block holds requirement state and ordering, and the reconciliation rules where
the four same-day specifications conflict.

- [Runtime v2 and game-operator breaking authority](plans/2026-08-09-runtime-hardening-rewrite.md)
  — the design authority.
- [W2-W7 reconciliation](plans/2026-08-10-w2-w7-reconciliation.md) — it won
  where one of the four work-item specifications disagreed with it, and all four
  have landed, so it is now the record of those rulings rather than reading
  ahead of implementation.
- [Requirement and migration map](plans/2026-08-09-runtime-migration-report.md)
- [Architecture](ARCHITECTURE.md)
- [Current execution checklist](TODO.md)
- [Independent review outcome, 2026-08-10](reviews/2026-08-10-runtime-hardening-review.md)
  — both reviews returned FAIL twice; every finding is now closed except B-F4,
  which stays accepted with a stated reason. A-F8 was accepted and then closed
  the same day by W8.
- [The next block after runtime hardening](plans/2026-08-10-next-block.md) —
  the requirement-by-requirement map. **Its requirement block closed on
  2026-08-11, all 42 of it**: every `REQUIRED_CORE` requirement is implemented,
  **40** own a per-requirement behavioural gate that closes them, and `a03`/`a05`
  own an aggregate gate with no per-requirement ID. What remains is W9, W11,
  W12's second half and that naming work; §6 is the list.
  *(Corrected 2026-08-11 against `07abc3e`, which reopened `a07` on a misreading
  of its acceptance text and left this line at 39 of 42: it substituted `a07`'s
  需求 sentence for the first of its two 验收 clauses and read that clause as
  unimplemented when it was only untested. `a07` closed again the same day in
  `bed456f`, extending `contract-agent-a07` to run the schedule its first
  clause needed. §6.1 is why the false reopening took twenty hours, and §2 is
  the full record.)*
- [Consumer attestation](plans/2026-08-11-consumer-attestation.md) — the nine
  `EXTERNAL attest-consumer-dNN` IDs, specified 2026-08-11: what a consumer
  produces, what binds it, and what upstream may and may not refuse. Proposal
  only; six questions await a ruling.
- [A project is a directory of data](plans/2026-08-11-project-as-data.md) — the
  correction that follows from "uf-chaos depends only on umbraflow's compiled
  binary": the exported C++ provider surface is replaced by a project directory
  format and a host-side loader, and conformance becomes a second shipped
  binary. A consuming project ends with no C++ at all. All ten questions are
  ruled in §7.0, and the framework switch landed at `974396e`: `provideProject`
  and the provider header are gone, and the suite takes `--project <directory>`.
- [The provider surface, measured](plans/2026-08-11-project-as-data-inventory.md)
  — the evidence that design is sized from, measured at `6bfe1d6`:
  `ProvidedProject`'s thirty leaves, reached 106 times, of which twenty-three are
  already data, five are C++ authorities and two are live-behaviour recorders.
  Its §A.1.1 is much of why the correction was worth making: **both in-tree
  exemplars validated nothing**, substituting hardcoded byte-string allowlists
  for the two validators `project-plugin.hpp:90-94` demands, so the framework's
  own two runs stayed green against a validator that accepted non-canonical
  bytes. `974396e` deleted both exemplars, and `deployment::loadProject` now
  builds every validator from the project's own schema bytes.
- [Recognition, measured against the real game](plans/2026-08-12-recognition-measured.md)
  — 76 real captures through the framework's own resolver, no game running, which
  established that this system **can** be validated offline. It refutes the
  design it was sent to verify: of 196 ambiguous key classes, 85 are separated by
  no field anywhere in the content graph and eight are byte-identical rows, so
  weighted scoring returns a tie however it is tuned. **Build neither the weights
  nor the alias tables.** The key names fields that are not on screen; option
  button labels are, and lift uniqueness from 278 to 523.
- [Project content at scale](plans/2026-08-12-project-content-at-scale.md) — what
  one `derive` actually reads, measured. Its own numbers refuted its container
  proposal, and a compact projection shipped instead. Its lasting finding is the
  one it was not sent for: **no observed string reaches the plugin at all.**
- [What the pure plugin environment owes](plans/2026-08-12-plugin-pure-helpers.md)
  — the plugin now receives a decoded frozen value rather than bytes, for its
  call input and for `artifact.read` alike, and `plugin_environment_hash` pins
  what wraps every call together with the versioned contract each published
  function answers to. Also the answer to whether the schemas are worth their
  complexity: keep all three categories, delete about 60% of the project payload
  schemas' bytes and none of their checks.
- [The journal record binding](plans/2026-08-11-journal-record-binding.md) —
  landed 2026-08-11. The `journal_events` and `project_state` rows now carry the
  member names of the journal records they store, the framework validates
  `JR:JournalProvenance` itself instead of delegating it, and `contract-state-s06`
  and `contract-agent-a04` bind each stored row to the schema's `required` list.
  Read it before touching either table or that schema file: the Operator DDL
  fingerprint moved and databases from before it are refused at open.
- [Historical pre-rewrite work queue](WORKLIST.md) — retained evidence only,
  not an implementation queue.

## Current plans

The first six below were the reading order; the dated decisions after them build
on it. Two of the six, the annotation model and layer ownership, were superseded
on 2026-08-11 and 2026-08-12 and are kept for their record rather than for
reading order.
[`plans/README.md`](plans/README.md) carries each plan's status and is the
canonical listing — this section is its short form, and it lists the 2026-08-09
rewrite design set that this section does not. Everything else is archived.

- Target form — three layers plus the Agent operator (approved 2026-08-01):
  [Three layers and the Agent operator](plans/2026-08-01-three-layers-and-agent-operator.md)
- Superseded 2026-08-12, kept as the record of a decision that was real when
  made (ruled 2026-07-31): element and page did leave C++ for trusted Luau, and
  the runtime model is now defined jointly by
  [`schema/umbraflow-runtime-v2.schema.json`](../schema/umbraflow-runtime-v2.schema.json),
  the normative form, and
  [Runtime model contract v1](plans/2026-08-09-runtime-model-contract.md), the
  field-level prose. Its §九 retirement list is executed and owes nothing, and
  §十's page-graph shape is answered by the schema's `transitions`; the two §十
  questions nothing answers are named in [`plans/README.md`](plans/README.md):
  [Script-owned page model](plans/2026-07-31-script-owned-page-model.md)
- Current product direction:
  [Product form and Roadmap](plans/2026-07-21-product-form-and-roadmap.md)
- Current task-system architecture (approved 2026-07-29; its layer-one boundary
  is amended by the script-owned page model above):
  [Three-layer task system](plans/2026-07-29-three-layer-task-system.md)
- Superseded 2026-08-11, kept as the record of a decision that was real when
  made (approved 2026-07-31): two of the three schemas it claimed to have landed
  under never existed, and the current annotation-model decisions are the three
  authorities under "Start here":
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
  — archived 2026-08-01 while still owing one thing. Its §6 ruling, that four
  `core` facilities be run through `evaluate-core-capability`, has been W12 of
  [the next block](plans/2026-08-10-next-block.md) since 2026-08-11; the review
  itself is left as written.

The knowledge base (`docs/knowledge/`) was deleted on 2026-08-01: with the code
framework mid-migration it was pure maintenance burden. Reusable failure
knowledge stays in [Pitfalls](pitfalls/README.md).

## Repository guidance

- [Domain glossary](../CONTEXT.md)
- Exported Operator conformance suite: what a consuming repository writes is a
  project directory, which it runs the shipped binary against as
  `umbra-flow-conformance --project <directory>`. The directory format is
  specified in §2 of
  [A project is a directory of data](plans/2026-08-11-project-as-data.md), the
  loader that reads it is
  [`modules/deployment/source/deployment/project-directory.hpp`](../modules/deployment/source/deployment/project-directory.hpp),
  and `examples/umbraflow` and `examples/arcana-expedition` are two such
  directories. Added 2026-08-10, renamed from `contract-suite/` on 2026-08-11,
  and stopped being a C++ surface a consumer implements the same day; see
  [Architecture](ARCHITECTURE.md) for where it sits.
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
