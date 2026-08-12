# Documentation Index

Four questions, in order. A newcomer should reach the current contract, the
current phase and the open blockers without reading anything historical.

This file is a reading order and **not** a catalogue.
[`plans/README.md`](plans/README.md) is the canonical per-plan status listing and
records what was archived and when; `archive/` holds the evidence.

## 1. The current contract

- [Runtime v2 and game-operator breaking authority](plans/2026-08-09-runtime-hardening-rewrite.md)
  — the frozen design authority. It overrides every conflicting clause anywhere
  else, and no compatibility implementation is permitted.
- [`schema/umbraflow-runtime-v2.schema.json`](../schema/umbraflow-runtime-v2.schema.json)
  — the normative runtime model, with
  [the runtime model contract](plans/2026-08-09-runtime-model-contract.md) as its
  field-level prose.
- [A project is a directory of data](plans/2026-08-11-project-as-data.md) — what
  a consuming project *is*: a directory, not a C++ library. A consumer compiles
  nothing and runs `umbra-flow-conformance --project <directory>`.
- [Architecture](ARCHITECTURE.md) — module ownership and the deliberate absences.
- [Domain glossary](../CONTEXT.md) — the terminology authority. A rename lands
  there first.

## 2. The current phase

- [The next block after runtime hardening](plans/2026-08-10-next-block.md) — the
  requirement-by-requirement record. **Its requirement block closed on
  2026-08-11, all 42 of it**: every `REQUIRED_CORE` requirement is implemented,
  40 own a per-requirement behavioural gate, and `a03`/`a05` own an aggregate
  gate with no per-requirement ID. **There is no open requirement.** What remains
  is listed below.
- [Current execution checklist](TODO.md) — the gate rows still owed. What was
  ticked, and the long record of corrections to those ticks, is
  [the correction record](archive/plans/2026-08-12-todo-correction-record.md).

## 3. What is still blocking

Nothing below is a design question. In rough order of what stops what:

- **W11, `clang-analysis`.** It blocks the branch rather than the design:
  `linux-analysis` is a required CI job and has never been seen to pass. Done
  when a build reports nothing **and states how many objects it analysed**.
- **This branch has never been through CI at all**, having never been pushed, so
  no ticked box in the checklist has been re-run anywhere but one Windows host.
- **W9, the adversarial review round over W2-W7.** The 2026-08-10 round ended
  before W2 landed; nothing has reviewed the five landings that carry the
  block's actual requirement closures.
- **The G0 review gate.** Both independent reviews returned FAIL, twice; every
  finding in [the runtime hardening review](reviews/2026-08-10-runtime-hardening-review.md)
  is now closed or accepted with a stated reason, but the box turns on a
  re-review verdict that does not yet exist. That review therefore stays live.
- **Four assertions the tree contradicts**, in
  [the carried debt ledger](plans/2026-08-12-carried-debt-ledger.md): the
  `identity_hash` invariance, the snapshot-join redundancy claim, `a01`'s event
  stream, and `p03`'s offer side. Two of them qualify requirements marked closed.
- **Findings with no owner in either tree** —
  [cross-repository drift](plans/2026-08-11-cross-repository-drift.md) F-3, all
  seven items of F-12, and its §1 ruling that the four executable specification
  resolutions require a co-versioned bundle v2.0.
- **Six questions awaiting a ruling** in
  [consumer attestation](plans/2026-08-11-consumer-attestation.md) §10.
- **W12's second half**, the retroactive review of the 15 files that entered
  `core` in the 2026-07-20 template import, and **per-requirement IDs for `a03`
  and `a05`**, which is naming work.
- **The real dual-game attestation is `EXTERNAL / NOT_RUN`** and no fixture can
  move it.

## 4. How to migrate onto the current shape

- [Requirement and migration map](plans/2026-08-09-runtime-migration-report.md) —
  the requirement-to-owner/schema/CTest map and the inherited-baseline
  dispositions. No gate reads it, so verify it before trusting it.
- **Operator databases are refused at open, never migrated.** The DDL
  fingerprint is `sha256:500c07b10e…` over 23 tables and moved seven times in
  three days; an `operator-runtime.sqlite` from any earlier date is deleted and
  recreated rather than migrated.
- **A consumer stops writing C++ entirely.** §5 of
  [a project is a directory of data](plans/2026-08-11-project-as-data.md) is the
  ordered migration, and `examples/umbraflow` and `examples/arcana-expedition`
  are two project directories written the way a consumer writes its own.
- The read-only consumer bundle is **v1.13**, root `c8e559a1…ec6e5f0`, pinned in
  two independent copies that `check-spec-pins` holds to each other. Agreement
  between the two copies is not freshness.

## Before investigating any failure

[Pitfalls](pitfalls/README.md) is required reading before diagnosing anything,
and a new non-obvious root cause is recorded there once it is understood. Start
with [checks that cannot fail](pitfalls/checks-that-cannot-fail.md): its detector
matrix identifies what kind of evidence each false-green shape requires.

## Everything else

- [Plans — the canonical status listing](plans/README.md), including every
  archived document and the date it left.
- `archive/plans/` and `archive/reviews/` — the evidence. Each archived file
  states at its top either that nothing in it is still owed or where what it owes
  now lives; that precondition is in `CLAUDE.md`.
- The knowledge base (`docs/knowledge/`) was deleted on 2026-08-01 and the two
  ADRs under `adr/` on 2026-07-29. Decisions land in dated plans under `plans/`
  and terminology in [`CONTEXT.md`](../CONTEXT.md); a document still pointing at
  either is drift.
- [C++ coding skill](../.claude/skills/cpp-coding/SKILL.md),
  [safe C++ profile](../.claude/skills/cpp-coding/references/safety-profile.md),
  [core capability evaluation](../.claude/skills/evaluate-core-capability/SKILL.md),
  [git change management](../.claude/skills/manage-git-changes/SKILL.md).
