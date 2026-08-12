# The record of corrections to the upstream execution checklist

> **Archived 2026-08-12 from the header of `docs/TODO.md`. Nothing here is still
> owed.** Every paragraph below is a dated correction to a box that was already
> ticked, or to an earlier correction of one; not one of them opens work. They
> are kept verbatim because they are evidence — three of them record a
> requirement being closed, reopened on a misreading, and closed again within
> twenty hours, and that sequence is the reason
> [the next block](../../plans/2026-08-10-next-block.md) §6.1 exists.
>
> They were moved because the checklist had become mostly this: 118 lines of
> history above the first line of work still owed, so a reader looking for what
> is open read the record of what is closed first. What is still owed stayed in
> [`docs/TODO.md`](../../TODO.md), which points here.
>
> **The three standing facts these paragraphs carry are restated live** at the
> top of that file, because they qualify every tick in it: the ticks predate
> seven Operator DDL schema breaks and have not been re-run against them; the
> `linux-analysis` job does not compile and blocks the branch regardless of
> anything ticked; and both independent reviews returned FAIL, which is what the
> two unticked review boxes wait on.
>
> *(Relative links in the body are as they were written while this was live and
> were not repointed. A `plans/…` or `pitfalls/…` link resolves from `docs/`.
> The links in this note are correct.)*

> Amended 2026-08-09: this checklist is derived from the
> [v1.7 breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md)
> and replaces the former consumer-project queue. Work in this repository does
> not modify a game project.
>
> Amended 2026-08-10: boxes below are ticked against a build and a test run
> that actually happened -- the tree had never been compiled before this date.
> Both independent reviews returned FAIL; their closed and open findings are in
> [the review outcome](reviews/2026-08-10-runtime-hardening-review.md), which is
> what the two unticked review boxes now wait on.
>
> Amended 2026-08-10, later the same day: this checklist covers the rewrite, not
> what follows it. The remaining work is enumerated per requirement in
> [the next block](plans/2026-08-10-next-block.md), of which W1 (behavioural
> cases for `c02 c04 s03 s06 a04 a06`), W5 (Replay Bundle and both publication
> gates), W8 (runtime-artifact reclamation by database refcount, which closed
> A-F8) and the exported conformance suite landed on this date. Two consequences
> reach the boxes below: the Operator ledger DDL fingerprint changed, so an
> operator database written before this date is refused at open and recreated
> rather than migrated; and `ctest -N` now also lists
> `conformance-umbraflow` and `conformance-arcana`. Every tick below
> predates those changes and has not been re-run against them.
>
> The same day, W0's merge-readiness run found that the ticked Windows gate is
> not the whole picture. The three sanitizer presets pass 62/62 with the
> sanitizers proved active, but the required `linux-analysis` CI job does not
> compile: `-Wunsafe-buffer-usage` and clang-tidy under `WarningsAsErrors: '*'`
> produce well over a hundred fatal diagnostics. That is tracked as W11 in
> [the next block](plans/2026-08-10-next-block.md) and blocks merging this
> branch regardless of anything below.
>
> Amended 2026-08-11: W3 (`4b955de`) and W2 (`848e390`) landed, closing `s01`,
> `s02`, `s04`, `c05` and `c08`. Three consequences reach the boxes below. The
> gate map moved again — 52 gates over 42 requirements, 33 `contract-*` and 19
> `schema-*` — and the migration report drifted a second time for the same
> reason as R3-F3, because both landings registered their cases before updating
> it; it has been repaired. The Operator DDL fingerprint moved twice more, to
> `sha256:12f64bfff305c30c716fbd5bdc9934a17140dfe4e127b5bce2ec7a10ecd309e4`
> over 20 tables, and `848e390` deleted the `runtime_publications` table that
> R3-F2 found unobservable, so the G2/G4 ticks predate a third schema break.
> And the block's mutations were executed for the first time: three of W2's
> stay green and are unresolved defects in the tests, carried in
> [the next block](plans/2026-08-10-next-block.md) §2 and in
> [checks that cannot fail](pitfalls/checks-that-cannot-fail.md).
>
> Amended 2026-08-11, after the block closed. W4 (`e64c143`, `25f57f9`), W6
> (`93698b4`) and W7 (`c23efd3`) landed, closing `c03`, `p01`, `p02`,
> `p03`, `a01` and `a02` — every `REQUIRED_CORE` requirement is now implemented.
> Four consequences reach the boxes below. The gate map is **59 gates over 42
> requirements, 40 `contract-*` and 19 `schema-*`**, and a green run was 83
> registered tests on that date — it is 86 as of 2026-08-11, the three added by
> `test-json`, `test-deployment` and the `open` verb's surface pin, none of which
> changes the gate map above. `a03` and `a05` are the only requirements without a
> per-requirement behavioural ID; both are implemented and both run under the
> aggregate `test-annotate-backend`, so that is a naming gap and not a coverage
> gap. The Operator DDL fingerprint moved four more times and is now
> `sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`
> over 23 tables, so every G2/G4 tick predates seven schema breaks; an operator
> database from any earlier date is refused at open and deleted, never migrated.
> And the migration report drifted a third, fourth and fifth time for the same
> structural reason — no gate reads it — which is now recorded in the report
> itself.
>
> Corrected 2026-08-11 (`07abc3e`), on two counts this paragraph got wrong.
> **`a07` was listed as closed by W4 and it is not**; it is reopened. Its
> acceptance text has two clauses, `contract-agent-a07` proves the second, and
> the first — that a human takeover and Host delivery share one target
> serialization — is implemented by nothing: the takeover transaction and
> `TaskHost`'s fence have no call edge between them in production or in test.
> **39 of 42 requirements are closed by a behavioural gate, not 40.** The gate
> map above is unchanged and correct: 40 `contract-*` gates are registered and
> green, and one of them closes half a requirement. The reopened row and the two
> ways it closes are in [the next block](plans/2026-08-10-next-block.md) §2. The
> fingerprint above also read `bda31e4b18…` and was a seventh break out of date:
> `07abc3e` renamed eight DDL columns to `controlled_target_id`, the only
> spelling now, which changes the canonicalized DDL text without changing a table
> name — hence the same 23 tables.
>
> Corrected 2026-08-11, once more: the fingerprint above also read
> `be80aca714…` before this block. Four DDL columns were renamed so that
> `journal_events` and `project_state` — both journal records — carry
> `$defs.JournalEvent` and `$defs.ProjectState`'s member names:
> `canonical_event` to `opaque_project_payload`, `canonical_provenance` to
> `provenance`, `state_schema_hash` to `project_state_schema_hash` and
> `canonical_state` to `canonical_opaque_payload`. No table added or dropped,
> hence the same 23 tables. See
> [journal record binding](archive/plans/2026-08-11-journal-record-binding.md).
>
> Corrected 2026-08-11 (`bed456f`), reversing the correction two paragraphs
> above: the reopening at `07abc3e` misread the acceptance text it was reopened
> against. The frozen bundle's row puts both consequences inside `a07`'s 验收,
> not one in its 需求 and one in its 验收: `contract-agent-a07` already proved
> the second (an in-flight dispatch is explicitly reported), and the first —
> `reserveDispatch`'s live-lease predicate, `requireLiveLease`, run inside the
> same `BEGIN IMMEDIATE` serialization `takeoverLease` commits in — was already
> implemented before the reopening. What was missing was not a call edge but a
> test that ran the schedule: no case took over and then attempted a
> reservation on the displaced lease. `contract-agent-a07` was extended, not
> joined by a second case, and both halves are now falsified by mutation.
> **42 of 42 `REQUIRED_CORE` requirements are closed by a behavioural gate**;
> the gate map is unchanged, 40 `contract-*` and 19 `schema-*`. See
> [the next block](plans/2026-08-10-next-block.md) §2, which is the current
> record — the two paragraphs above it record the misreading and its
> correction, not the requirement's state.
>
> Amended 2026-08-10, after the third adversarial round. It returned FAIL with
> 17 findings — [the record](archive/reviews/2026-08-10-third-round-review.md) — so the
> two unticked review boxes below wait on it too. Three corrections to the
> paragraphs above. The G0 tick "map every requirement to an exact CTest ID"
> holds again only as of today's repair: `dcc43b5` renamed 14 gate IDs to
> `schema-*` without updating the migration report, which then named CTests
> `ctest -N` cannot produce; the report now carries all 47 gates — 28
> `contract-*` and 19 `schema-*` — over 42 requirements. `ctest -N` gained four
> `CONFORMANCE` aggregates rather than two: `test-contract-operator` and
> `test-contract-runtime` as well as `conformance-umbraflow` and
> `conformance-arcana`. And W11 is a scope rather than a count — "well over a
> hundred" was W0's reading before `603b0b0`, `cec8898` and `6f8d3a8` cleared
> everything outside `modules/operator`, `conformance` and `tests/operator`.
