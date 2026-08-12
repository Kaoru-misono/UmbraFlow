# Plans

The current authorities live here, and nothing else does. This is the canonical
plan location required by `CLAUDE.md` and `AGENTS.md`, and **this file is the
canonical per-plan status listing** — [`docs/INDEX.md`](../INDEX.md) is a short
reading order and deliberately does not list every plan. Superseded and completed
plans are under [`docs/archive/plans/`](../archive/plans/); closed reviews are
under [`docs/archive/reviews/`](../archive/reviews/). Historical research there
must not override the authorities below.

Every archived document is listed here with the date it left, so nothing becomes
unfindable by being moved. An archived document states, at its top, either that
nothing in it is still owed or where what it owes now lives; that is the
archiving precondition in `CLAUDE.md`.

## Current authority

Reading order: target form, then layer ownership, then the model semantics.

- [Runtime v2 and game-operator breaking rewrite](2026-08-09-runtime-hardening-rewrite.md) —
  frozen breaking authority for Annotation/Observation Runtime v2, Host Receipt/delivery ownership,
  Operator Core, and the multi-game ProjectPlugin boundary. Its pinned product design overrides every
  conflicting Page/Element/Hit/UFR, Context, direct-run, caller-measurement, caller-effect, and
  pre-Operator action clause below. No compatibility implementation is permitted.
  It is also the document `scripts/check_spec_bundle.py` reads: its `:10-18`
  digest lines are one of the gate's two independent copies of the v1.13 bundle
  pin, so an edit there is a gate change.
- [Runtime v2 migration report](2026-08-09-runtime-migration-report.md) —
  reproducible inherited baseline, KEEP/REWRITE/DELETE dispositions, and the exact
  requirement-to-owner/schema/CTest map. **No gate reads it**, which is recorded
  in the report itself and is why it is always the side that drifts.
- [The next block after runtime hardening](2026-08-10-next-block.md) --
  **its requirement block closed on 2026-08-11, all 42 of it**, and it is still
  the live record of what remains. It established that the tree was mid Phase 2A
  rather than past it, that this branch has never been pushed and so has never
  been through CI, and ordered the deferred work by dependency. W1, W5, W8 and
  the conformance suite it raised as unowned landed on 2026-08-10; W3, W2, W4, W6
  and W7 landed on 2026-08-11, closing every `REQUIRED_CORE` requirement.
  `a07` was reopened on 2026-08-11 (`07abc3e`) on a misreading and closed again
  the same day (`bed456f`); §6's item 0, which outlived that closure by a day,
  was struck on 2026-08-12. **There is no open requirement.** What remains is W9
  (the review round over W2-W7, now unblocked), W11 (`clang-analysis`, which
  blocks the branch), W12's retroactive import review, and per-requirement IDs
  for `a03` and `a05`.
- [What the archived W-series specifications and reviews still owe](2026-08-12-carried-debt-ledger.md)
  — added 2026-08-12, and the live owner of every ruling that left the live set
  with the six documents archived that day. Forty-odd rows, each naming its
  origin section and its state in the tree. Four of them are assertions the tree
  contradicts and that are still readable somewhere a later agent will trust:
  `identity_hash` invariance, the snapshot-join redundancy claim, `a01`'s event
  stream, and `p03`'s offer side. Read it before trusting a W-series document.
- [Cross-repository drift audit](2026-08-11-cross-repository-drift.md) — **report
  only, and listed here for the first time on 2026-08-12**; it had been in
  `docs/plans/` since 2026-08-11 indexed by nothing, which is an instance of its
  own §4, "authorities that no gate reads". Fifteen framework findings and
  thirteen bundle self-contradictions, measured against the frozen v1.9 bundle
  and left at that read deliberately. F-13 and F-14 are applied; F-4, F-8 and
  F-11 have live owners; **F-3, all seven items of F-12, and the §1 ruling that
  the four executable specification resolutions require a co-versioned bundle
  v2.0 have no owner in either tree.** It cannot be archived until they do.
- [Consumer attestation](2026-08-11-consumer-attestation.md) — **specification
  proposal, nothing implemented.** What `attest-consumer-d01`-`d09` are, what
  each of the nine requirements must attest, who signs and what that does and
  does not prove, where a set is recorded and how it is refused, and how it
  relates to `attest-dual-game-p05` and the exported conformance suite. It changes
  no schema and no compiled hash here; six questions in its §10 need a ruling.
  It also carries the 2026-08-11 correction that `D-09` is `PHASED` and that
  `C-11` and `A-04` carry `PROJECT_CONTRACT`.
- [A project is a directory of data](2026-08-11-project-as-data.md) — **ruled in
  full, and the framework half has landed.** `ProvidedProject` member by member
  and which of its thirteen members a data-only project could never supply; the
  project directory format, its two root documents and the vocabulary document
  that had to be invented; why conformance is a second binary rather than a
  subcommand; why the plugin mechanism was already finished and the loader was
  the only missing piece; what dies in both trees, and the eight-step order in
  which uf-chaos's `contract/` may finally be deleted, leaving a project with no
  compiler and no CMake. All ten questions are ruled in §7.0, and the framework
  switch landed at `974396e`: `provideProject` and the provider header are gone,
  and the suite takes `--project <directory>`.
- [The provider surface, measured](2026-08-11-project-as-data-inventory.md) —
  the independent measurement the document above is sized from, taken at
  `6bfe1d6` by an agent that could not see it. Every `ProvidedProject` leaf,
  where it was reached and by what; which of the three provider implementations
  built each one from committed bytes and which from a literal; what uf-chaos's
  828-line schema evaluator implements against what its schemas use. Two results
  governed the migration order rather than merely describing it: **neither
  in-tree exemplar parsed a byte**, so nothing here could test the correction
  until something did, and the eight suite cases most entangled with C++
  construction — including all four that exist to prove the five authorities
  cannot be forged — close no requirement at all. `974396e` settled the first:
  both exemplars are deleted, and `deployment::loadProject` builds every
  validator from the project's own schema bytes.
- [Recognition, measured against the real game](2026-08-12-recognition-measured.md)
  — **measurement, and the strongest refutation this branch has produced.** 76
  real captures driven through `engine::IFrameSource` and a real `TaskHost`, 152
  Host activations in 25 seconds with no game running — which is the practical
  finding, because it sets how fast any of this can be iterated. Three captures
  resolve and 73 return `no_scene_candidate`, exactly matching a two-surface
  model asked to name 53 pages, with no false positive or negative. The
  recognition rule cannot be applied to a capture at all, for four independent
  reasons, none of them content ambiguity. And of 196 ambiguous key classes, 85
  are separated by no field anywhere in the graph — so a weighted scorer returns
  a tie however it is tuned, and the `RecognitionPack`'s weights, thresholds and
  alias tables should not be built. Read it before designing any recognition.
- [Project content at scale](2026-08-12-project-content-at-scale.md) — what one
  `derive` actually reads, measured against the real 70 MB graph. Its container
  recommendation was refuted by its own numbers and a compact projection shipped
  instead; what survives is the finding it was not sent for, that `derive`
  receives no observed string, so a perfect index has nothing to look up by.
- [What the pure plugin environment owes](2026-08-12-plugin-pure-helpers.md) —
  **largely landed.** The plugin receives a decoded frozen value rather than
  bytes, which removed every `string.match` over a serialised envelope — one of
  which matched the first `tool_name` anywhere in the document — and shrank one
  fixture plugin by 63%. `artifact.read` answers the same way, so a registered
  artifact is a JSON document the host parses once at registration and hands
  over frozen, built at most once per VM. `plugin_environment_hash` pins the
  bridge that wraps every plugin call and was covered by nothing; it now also
  pins what each published function returns, because a preimage over member
  names alone left that change invisible. Its `canon.encode` was not built:
  no caller survived the ruling that the host mints instance ids. It also carries
  the answer to whether the schemas earn their complexity — keep all three
  categories, delete about 60% of the project payload schemas' bytes and none of
  their checks, since 3,584 of 5,996 lines are one `$defs` block repeated eleven
  times and the framework causes that.

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
  that it described was removed that day, unreferenced by any target. **T03, T07
  and T11 are open**, and T07 is the live owner of the element-granularity
  question the script-owned page model raised.
- [Claude handoff](2026-08-09-claude-handoff.md) — the takeover brief for this
  branch and its hard boundaries. **It stays live because it is the only
  statement anywhere of this branch's writable-repository boundary**: this
  repository is the only writable tree, and the consumer project is readable as a
  specification source and never written. Its §9 delivery criteria still list a
  contract-gate count that its own §8.3 note says is no longer usable as a count.

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
  2026-07-31, **superseded 2026-08-12** and kept as the record of that decision. The runtime
  model is now defined jointly by
  [`schema/umbraflow-runtime-v2.schema.json`](../../schema/umbraflow-runtime-v2.schema.json),
  the normative form, whose exact bytes the runtime-model file reader in `modules/task/` pins as
  `k_runtimeModelSchemaHash`, and by
  [the runtime model contract](2026-08-09-runtime-model-contract.md), the field-level prose,
  frozen for P0. What it ruled outlives it: `element` and `page` are not C++ types, C++ keeps
  the primitives plus the ticket ledger that makes acting on an observation safe, and the model
  is a project file the trusted framework owns. **§九 owes nothing** — `modules/annotation`,
  `entry/workbench`, `entry/authoring`, `CapabilitySurface`, `resolvePage` and `findAction` are
  all absent from the tree, and the `docs/TODO.md` ticket that tracked the retirement went with
  the 2026-08-09 rewrite of that file. **§十's page-graph shape is answered in the schema**:
  `runtime_model` requires `transitions`, and `transition` / `transition_trigger` make an edge a
  declared move between two legal Surface stacks triggered by one named Binding action, which
  `modules/task/runtime/model.luau` validates on both stacks and on the trigger. It is answered
  differently from §十's own 2026-08-01 ruling: there is no `kind = navigate|push|pop` and no
  `via = key|spontaneous`, a Binding action is the only trigger, and layering is carried by the
  Surface stack and `covers` rather than by an edge kind. The `catch_all` flag that §十's
  2026-08-04 follow-up added does not exist either — the surface kinds are `scene`, `overlay`
  and `interrupt`, and every Surface must name a positive identity Binding, so an authored
  catch-all cannot be written at all. Two more of §十's five items are closed: the batch-matching
  cost, measured inside the document itself and ruled against a batch primitive, and the explicit
  interrupt-page kind, which exists as the `interrupt` surface kind. Element granularity in a
  scrolling grid is [the test matrix](2026-08-09-annotation-v2-test-matrix.md)'s T07, and is
  owned there.
  **It is not archivable, for two independent reasons.** Its last §十 item — the
  confirm-versus-recognise split — is answered nowhere and owned by nothing:
  `resolve_state` is the only entry, nothing verifies one expected Surface
  against one marker, and what to do after a failed confirmation is therefore
  moot rather than settled. And it is cited from source: `modules/engine/manifest.txt`,
  `modules/engine/source/engine/session.hpp` and `tests/engine/test-session.cpp`
  name this path, so moving it would break references in code that a
  documentation change may not edit.
- [Product form and Roadmap](2026-07-21-product-form-and-roadmap.md) — product direction,
  P0–P3 scope and exit criteria.
- [Three-layer task system](2026-07-29-three-layer-task-system.md) — developer-approved
  2026-07-29 authority for the task system: C++ guarantee layer, bundled trusted Luau
  framework, and project-owned tasks; the observation-cycle protocol, the `uf` script root,
  the merged `umbraflow-trace/v2` stream, and the staged implementation (stages 1–3 landed).
  Its layer-one clauses (`cycle_page` / `cycle_find`, the `engine -> annotation` edge) are
  amended by the script-owned page model above.
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
  game-specific measuring and annotation is in the consumer project's `MAP.md`.

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

## Archived, and when

Moving a document is not a way to close it. Each entry below states what it was
and what it still owed at the moment it left; the file itself carries the same
statement at its top.

### Archived 2026-08-12 — the W-series and what governed it

All four work items landed on 2026-08-11, so the four specifications and the
document that reconciled them stopped reading ahead of the implementation. Their
unexecuted rulings were carried into
[the carried debt ledger](2026-08-12-carried-debt-ledger.md) first; each file
names its own rows there.

- [W2 EffectivePlan authority](../archive/plans/2026-08-10-w2-effective-plan.md)
  — landed `848e390`. Owed `C-W2-1` to `C-W2-3`.
- [W3 Snapshot Coordinator](../archive/plans/2026-08-10-w3-snapshot-coordinator.md)
  — landed `7cef402` + `4b955de`. Owed `C-W3-1` to `C-W3-11`; the only one of
  the four with no "what it still owes" section of its own.
- [W4 delivery join](../archive/plans/2026-08-10-w4-delivery-join.md) — landed
  `e64c143` + `25f57f9`. Owed `C-W4-1` to `C-W4-9`. Its Q5 is the concession
  that died in place and left `a07` falsely closed for twenty hours.
- [W6 and W7 controller and Agent](../archive/plans/2026-08-10-w6-w7-controller-and-agent.md)
  — landed `93698b4` + `c23efd3`. Owed `C-W67-1` to `C-W67-11`, two of which
  qualify requirements its own `Closes:` line marks closed.
- [W2-W7 reconciliation](../archive/plans/2026-08-10-w2-w7-reconciliation.md) —
  the ruling record that governed all four. Owed `C-R-1` to `C-R-7`. **Its §7.2
  recommendation that the ledger source `runtimeGeneration` from
  `sessions.installed_generation` is false and is marked so at the
  recommendation** — those are two different quantities that only a fixture makes
  look like one.

### Archived 2026-08-12 — settled elsewhere

- [Third adversarial round](../archive/reviews/2026-08-10-third-round-review.md)
  — verdict FAIL over 17 findings, 2026-08-10. Twelve closed, four still owed
  (`C-R3-1` to `C-R3-4`), one accepted permanently. **Nine of the twelve closures
  had been recorded in no document at all** and are now in the ledger's §F. It
  had never been listed in this file or in `docs/INDEX.md` while it was live,
  although six documents cite it as their evidence.
- [Consumer onboarding](../archive/plans/2026-08-11-consumer-onboarding.md) —
  superseded in shape by project-as-data and kept for its measurement of what the
  deleted C++ provider surface cost a consumer. Owed `C-CO-1` to `C-CO-4`,
  including one ruling nobody executed and nobody made moot: the background-only
  product invariant must reach a consumer as an attestation.
- [The journal record binding](../archive/plans/2026-08-11-journal-record-binding.md)
  — landed 2026-08-11. Its §9 owed one edit to `CONTEXT.md`, which was applied on
  2026-08-12 before the move; nothing else was owed. The Operator DDL fingerprint
  it moved is `sha256:500c07b10e…` over 23 tables, and databases from before it
  are refused at open.
- [Annotation model — capabilities, holding, appearances](../archive/plans/2026-07-31-annotation-model-capabilities.md)
  — developer-approved 2026-07-31, superseded 2026-08-11, nothing owed. Two of
  its §二 conclusions survive under the current vocabulary and are stated in
  `CONTEXT.md` and the runtime v2 schema rather than only here: a pixel region is
  annotated once as a `ui_target` whose Bindings carry their own actions, and
  several appearances of one element are several Bindings on one `ui_target` told
  apart by `variant`. `Capabilities`, `Holding`, `exercised`, `Appearance` and
  `ElementId` are not types in the tree.
- [Pre-rewrite work queue](../archive/plans/2026-08-05-worklist.md) — was
  `docs/WORKLIST.md`; superseded by the 2026-08-09 breaking rewrite and retained
  as evidence. Nothing owed: what is still open in it is consumer-project work,
  or work against tooling the rewrite deleted.
- [The record of corrections to the execution checklist](../archive/plans/2026-08-12-todo-correction-record.md)
  — the 118-line block of dated amendments that stood at the head of
  [`docs/TODO.md`](../TODO.md) until 2026-08-12. Nothing owed: not one paragraph
  in it opens work. It is kept because it is evidence — three of its paragraphs
  record a requirement closed, reopened on a misreading and closed again inside
  twenty hours, which is the incident behind
  [the next block](2026-08-10-next-block.md) §6.1. The three facts in it that
  qualify every tick are restated live at the head of the checklist.

### Archived earlier

The moves before 2026-08-12 did not record their dates in one place, which is
what this section exists to stop. Where a date is known it is given.

- [Locked S0 annotation contract](../archive/plans/2026-07-22-annotation-design.md)
- [Luau task-model grill decisions](../archive/plans/2026-07-21-lua-task-model-grill-decisions.md)
  and [the decision package](../archive/plans/2026-07-21-lua-task-model-decision-package.md),
  with [its full transcript](../archive/plans/2026-07-21-lua-task-model-decision-package.FULL.md)
- [Lua task-model grill](../archive/plans/2026-07-20-lua-task-model-grill.md)
- [Luau integration plan](../archive/plans/2026-07-21-luau-integration-plan.md)
- [P0-B Luau hardening ledger](../archive/plans/2026-07-21-p0b-luau-hardening-ledger.md)
- [P0-B script layer](../archive/plans/2026-07-27-p0b-script-layer.md)
- [Safe C++ core plan](../archive/plans/2026-07-20-safe-cpp-core.md)
- [Post-port Win32 robustness](../archive/plans/2026-07-20-post-port-win32-robustness.md)
- [UI verification runbook](../archive/plans/2026-07-20-ui-verification-runbook.md)
- [Engine architecture](../archive/plans/2026-07-23-engine-architecture.md)
- [Page-centric authoring](../archive/plans/2026-07-26-page-centric-authoring.md)
- [Workbench UI redesign](../archive/plans/2026-07-27-workbench-ui-redesign.md)
- [Full-project review fixes](../archive/plans/2026-07-28-full-project-review-fixes.md)
- [Luau-first task system design draft](../archive/plans/2026-07-28-luau-first-task-system-design-draft.md)
  — superseded in full on 2026-07-29 by the three-layer task system.
- [Annotation backend branch review](../archive/reviews/2026-07-22-annotation-backend-review.md),
  [Luau-first draft review](../archive/reviews/2026-07-28-luau-first-draft-review.md)
  and [full-project architecture review](../archive/reviews/2026-07-27-full-project-review.md)
  — closed and moved 2026-08-01.
- [Repo-wide C++ simplification sweep](../archive/reviews/2026-07-25-simplify-sweep.md)
  — **archived 2026-08-01 in `eb1d205` while still owing one thing, which is why
  the archiving precondition exists.** Its §6 ruling, that four `core` facilities
  be run through `evaluate-core-capability`, went with it and stayed inert for
  seventeen days; it has been W12 of [the next block](2026-08-10-next-block.md)
  since 2026-08-11. The review itself is left as written — the pointer runs one
  way, from the next block to there.

A plan should be self-contained: it should include enough research, file paths,
implementation steps, and verification commands for another agent to execute it
without rediscovering context.

When a plan is completed, superseded, or verified, archive it with the archive
workflow rather than leaving it mixed into the authorities above — and carry what
it owes into a live owner first, or it is not archivable.
