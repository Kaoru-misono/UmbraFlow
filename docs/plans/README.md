# Plans

The current authorities live here, and nothing else does. This is the canonical
plan location required by `CLAUDE.md` and `AGENTS.md`. Superseded and completed
plans are under [`docs/archive/plans/`](../archive/plans/); closed reviews are
under [`docs/archive/reviews/`](../archive/reviews/). Historical research there
must not override the authorities below.

## Current authority

Reading order: target form, then layer ownership, then the model semantics.

- [Runtime v2 and game-operator breaking rewrite](2026-08-09-runtime-hardening-rewrite.md) —
  frozen breaking authority for Annotation/Observation Runtime v2, Host Receipt/delivery ownership,
  Operator Core, and the multi-game ProjectPlugin boundary. Its pinned product design overrides every
  conflicting Page/Element/Hit/UFR, Context, direct-run, caller-measurement, caller-effect, and
  pre-Operator action clause below. No compatibility implementation is permitted.
- [Runtime v2 migration report](2026-08-09-runtime-migration-report.md) —
  reproducible inherited baseline, KEEP/REWRITE/DELETE dispositions, and the exact
  requirement-to-owner/schema/CTest map.
- [The next block after runtime hardening](2026-08-10-next-block.md) --
  **its requirement block closed on 2026-08-11 except `a07`**, and it is still
  the live record of what remains. It established that the tree was mid Phase 2A
  rather than past it, that this branch has never been pushed and so has never
  been through CI, and ordered the deferred work by dependency. W1, W5, W8 and
  the conformance suite it raised as unowned landed on 2026-08-10; W3, W2, W4, W6
  and W7 landed on 2026-08-11, closing every `REQUIRED_CORE` requirement but one.
  Open: `a07`, reopened on 2026-08-11 (`07abc3e`) because its gate proves one of
  its two acceptance clauses and nothing implements the other — 39 of 42 closed,
  not 40; W9 (the review round over W2-W7, now unblocked), W11
  (`clang-analysis`, which blocks the branch), W12's retroactive import review,
  and per-requirement IDs for `a03` and `a05`. W12 was added on 2026-08-11 — the `core` admission debt
  that the 2026-07-25 sweep ruled and that was archived with the review before
  anyone ran it — and `bcc3171` answered its first half the same day.
- Four of that plan's work items were written out in full on 2026-08-10, each
  specification only. They do not open rulings of their own; they are the next
  block's rows at implementation depth, and they are superseded by it wherever
  they disagree. **All four have now landed and all four are kept as the
  pre-landing record**, each carrying a dated note saying what differs from the
  tree, which of its clauses the implementation refused, and what it still owes.
  None of them is guidance any longer.
  [W2 EffectivePlan authority](2026-08-10-w2-effective-plan.md) — landed
  `848e390` --
  [W3 Snapshot Coordinator](2026-08-10-w3-snapshot-coordinator.md) — landed
  `7cef402` + `4b955de` --
  [W4 delivery join](2026-08-10-w4-delivery-join.md) — landed `e64c143` +
  `25f57f9` --
  [W6 and W7 controller and Agent](2026-08-10-w6-w7-controller-and-agent.md) —
  landed `93698b4` + `c23efd3`.
- [The journal record binding](2026-08-11-journal-record-binding.md) — **landed
  record.** A consuming project read `schema/umbraflow-journal-v1.schema.json`
  and `ledger.cpp` side by side and reported five disagreements. Two were real:
  four DDL columns of the two tables that ARE journal records had drifted from
  those records' schema member names, `journal_events.canonical_event` worst of
  all because it named bytes that are not an event. Two were misreadings and the
  document says why. The fifth, `JournalProvenanceValidator`, is deleted and the
  framework now enforces `JR:JournalProvenance` itself. `contract-state-s06` and
  `contract-agent-a04` gained the assertion that binds each stored row to the
  schema's `required` list, which is what makes a schema-only edit able to turn
  a behavioural gate red. Fingerprint moved to `sha256:500c07b10e…`,
  still 23 tables; existing databases stop opening.
- [Consumer attestation](2026-08-11-consumer-attestation.md) — **specification
  proposal, nothing implemented.** What `attest-consumer-d01`-`d09` are, what
  each of the nine requirements must attest, who signs and what that does and
  does not prove, where a set is recorded and how it is refused, and how it
  relates to `attest-dual-game-p05` and the exported conformance suite. It changes
  no schema and no compiled hash here; six questions in its §10 need a ruling.
  It also carries the 2026-08-11 correction that `D-09` is `PHASED` and that
  `C-11` and `A-04` carry `PROJECT_CONTRACT`.
- [W2-W7 reconciliation](2026-08-10-w2-w7-reconciliation.md) — the four above
  were written in parallel by agents who could not see each other. This resolves
  every conflict between them, lists the cross-assumptions one makes that another
  does not satisfy, fixes the landing order, and unions their `ledger.cpp` DDL
  changes so the schema fingerprint is recomputed once per landing. It governed
  all four and they have all landed, so it is now a ruling record rather than
  something to read before implementing. Its §6.3 carries a row per landing;
  **its §7.2 recommendation that the ledger source `runtimeGeneration` from
  `sessions.installed_generation` is false and is marked so at the
  recommendation** — those are two different quantities that only a fixture
  makes look like one.

## The 2026-08-09 rewrite design set

The rewrite was specified over one day in five more documents. They are not
authorities — the frozen rewrite above wins wherever they disagree with it — but
several field-level and behavioural contracts are written down nowhere else, so
they stay here rather than being archived. Listed 2026-08-11, having been in
`docs/plans/` and absent from this index since they were written.

- [Runtime annotation and Agent model](2026-08-09-runtime-annotation-and-agent-model.md) —
  design proposal: annotation as offline model building and compilation, and the
  deployment boundary that follows from it.
- [Runtime model contract v1](2026-08-09-runtime-model-contract.md) — frozen for
  P0 implementation: the field-level contract, and the document that drops
  `Page`, `Element`, `Reference`, `CapabilitySet`, `holding`, `exercised`,
  `screen` and `expect` from the runtime model.
- [Annotation-system parallel work breakdown](2026-08-09-annotation-agent-work-breakdown.md) —
  the P0-P8 package split and each package's write set.
- [Annotation-v2 test matrix](2026-08-09-annotation-v2-test-matrix.md) — eleven
  behaviour contracts, T01-T11. Since 2026-08-11 it is their only record: the
  fixture data under `tests/annotation/` and `tests/fixtures/annotation-v2/`
  that it described was removed that day, unreferenced by any target.
- [Claude handoff](2026-08-09-claude-handoff.md) — the takeover brief for this
  branch and its hard boundaries.

`2026-08-09-runtime-migration-baseline.manifest.json` and
`-disposition.manifest.json` are the migration report's machine-readable data,
not separate plans.

## Retained predecessor references

The documents below preserve prior decisions and measurements. They are
superseded wherever they conflict with the current authority above.

- [Three layers and the Agent operator](2026-08-01-three-layers-and-agent-operator.md) —
  developer-approved 2026-08-01 **target form**: the C++/framework/business split, the Agent
  as a third operator with its own front end, the run/explore trust modes, annotation as the
  inter-layer contract (four added rulings), the page / named-appearance / unnamed-appearance
  modelling trichotomy, and the full verb surface. Amends the three-layer plan below; does not
  overturn it.
- [Script-owned page model](2026-07-31-script-owned-page-model.md) — developer-ruled
  2026-07-31, reconciled with the target form in §十二, **migration in progress**. `element`
  and `page` move up to the trusted Luau framework; C++ keeps primitives only (`cycle_match`,
  `cycle_read`, `cycle_click`, project file I/O) plus the ticket ledger that makes acting on an
  observation safe. Amends the layer-one boundary of the three-layer plan and relocates the
  annotation model — its design conclusions stand, its implementation location does not. §九 is
  the retirement list; §十 holds the undecided page-graph shape that currently blocks layer two.
- [Product form and Roadmap](2026-07-21-product-form-and-roadmap.md) — product direction,
  P0–P3 scope and exit criteria.
- [Three-layer task system](2026-07-29-three-layer-task-system.md) — developer-approved
  2026-07-29 authority for the task system: C++ guarantee layer, bundled trusted Luau
  framework, and project-owned tasks; the observation-cycle protocol, the `uf` script root,
  the merged `umbraflow-trace/v2` stream, and the staged implementation (stages 1–3 landed).
  Its layer-one clauses (`cycle_page` / `cycle_find`, the `engine -> annotation` edge) are
  amended by the script-owned page model above.
- [Annotation model — capabilities, holding, appearances](2026-07-31-annotation-model-capabilities.md) —
  developer-approved 2026-07-31, **superseded 2026-08-11** and kept as the record of that
  decision. The entry here previously said its model conclusions remained authoritative and had
  landed under `umbraflow-authoring/v4`, `umbraflow-annotations/v3` and `umbraflow-trace/v2`.
  Only `umbraflow-trace/v2` is real, and it is untouched by this supersession; the other two
  are in no schema and in no source file. The runtime model is now
  `schema/umbraflow-runtime-v2.schema.json`. `Capabilities`, `Holding`, `exercised`,
  `Appearance` and `ElementId` are not types in the tree. Two of its §二 conclusions survive
  under the current vocabulary — a pixel region is annotated once as a `ui_target` whose
  Bindings carry their own actions, and several appearances of one element are several Bindings
  on one `ui_target` told apart by `variant`. Everything above about C++ types, CLI verbs and
  schema key names is superseded by the runtime hardening rewrite.
- [Agent front end and the exploration environment](2026-08-01-agent-front-end-and-exploration.md) —
  developer-authorised 2026-08-01, the implementation shape of the second Luau environment:
  which verbs are privileged, how the closure isolation is built, and the `annotation.*` trace
  vocabulary. Opens no ruling of its own; it pins the shape the target form above decided.
- [State layer and policy slots](2026-08-04-state-layer-and-policy-slots.md) —
  direction settled with five rulings answered 2026-08-04, **four phases none executed**. The
  `l2-v2` schema, `expected_presence`, the appearance gate, and the co-resolution matrix. Phase A
  landed the matrix on 2026-08-04; B depends on A's evidence, C on B, D on C plus the real machine.
- [Storing the evidence corpus](2026-08-04-evidence-storage.md) — four tiers for keeping
  screenshots out of version control. **Tier 4 shipped 2026-08-04** (`assets/screens` left git,
  `.git` 148 MB → 413 KB); tier 0 was retired by measurement; tiers 1–3 remain proposals.
- [Framework capabilities for full-map planning](2026-08-05-map-verbs-and-connectivity.md) —
  direction settled 2026-08-05, ordered and pending execution. Two new verbs (an atomic
  `drag(start, offset)`, a two-point connectivity read), one conditional item (kind enumeration,
  only if the expanded map page turns out not to sit on a regular grid), and a stitched-map
  evaluation separate from the screen matrix. It also rules three things OUT: a frame-difference
  primitive, general line-segment detection, and cross-cycle coordinate identity. The
  game-specific measuring and annotation is in `E:\umbraflow-projects\uf-chaos\MAP.md`.

## Proposals awaiting a decision

Nothing here is an authority. Each records measurement and a proposal; no part of
it has been approved, and no code has been changed on its account.

- [Luau coding standard — measurements and outline](2026-08-02-luau-coding-standard.md) —
  2026-08-02 survey of the 15 trusted framework modules across six dimensions, the
  outline of a standard to sit beside the C++ one, six questions deliberately left
  unruled, and 15 changes ranked by value. Three of them are defects rather than
  style, listed separately in [`docs/TODO.md`](../TODO.md). Every row is one
  approval decision; none are bundled.

## Retained reference

- [M0 demo port deviations](2026-07-20-m0-demo-port-deviations.md) — frozen real-machine
  acceptance reference pending parity and retirement.

A plan should be self-contained: it should include enough research, file paths,
implementation steps, and verification commands for another agent to execute it
without rediscovering context.

When a plan is completed, superseded, or verified, archive it with the archive
workflow rather than leaving it mixed into the authorities above.
