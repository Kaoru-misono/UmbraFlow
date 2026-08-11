# The journal records, their columns, and who enforces their provenance

Date: 2026-08-11
Branch: `design/annotation-system-v2`
Baseline audited and changed: `ab97e2d`

A consuming project built its reference implementation against this framework's
contract and, doing so, read `schema/umbraflow-journal-v1.schema.json` and
`modules/operator/source/operator/ledger.cpp` side by side — which nobody inside
this repository had done for these records. It reported five disagreements. Two
were real and are fixed here, one was a misreading caused by the real ones, one
was a misreading of what an envelope member carries, and one was already known
and is now closed. What follows is the ruling on each and what changed.

---

## 1. `$defs.ProjectState` is produced by framework code — it is a row

**Reported:** the definition is emitted by nothing, validated by nothing, and
read only as text by two gates, so it does not belong in the schema.

**Verdict: the consumer misread, and the real defect below is what made the
misreading available.** `schema/umbraflow-journal-v1.schema.json` describes the
three durable record kinds of a project journal, and this framework stores them
as SQLite rows rather than as JSON documents. `$defs.ProjectState` has eight
required members; the `project_state` row has eight columns, and they are the
same eight facts. The argument that would retire `ProjectState` retires
`$defs.JournalEvent` with it — nothing emits an eleven-member `JournalEvent`
document either, and `A-04` is gated on that definition.

What was true is that **nothing connected the schema file to the DDL**, so two
of `ProjectState`'s eight names and two of `JournalEvent`'s eleven had drifted
and no gate could notice. A reader who compares the two files and finds four
names that appear on one side only will reasonably conclude the definition is
unproduced. That is now a gate rather than an inference; see §6.

`$defs.ProjectInstance` is the exception and is stated as one: it names nine
members and the `project_instances` row carries four, the rest being assembled
from `project_registrations` and `project_state`. `contract-state-s06` asserts
that mismatch deliberately, as the positive control that the new comparison can
fail at all.

## 2. The DDL and the schema spelled one concept two ways — real, fixed

**Verdict: real, and the DDL was the drifted side.** The schema is the contract,
and on this point the DDL also disagreed with its own sibling table:
`project_registrations` already spelled the value `project_state_schema_hash`
while `project_state` called it `state_schema_hash`.

| Was | Is | The schema member it stores |
|---|---|---|
| `project_state.state_schema_hash` | `project_state_schema_hash` | `ProjectState.project_state_schema_hash` |
| `project_state.canonical_state` | `canonical_opaque_payload` | `ProjectState.canonical_opaque_payload` |

## 3. `journal_events.canonical_event` named bytes that are not an event — real, fixed

**Verdict: real, and the sharpest of the five.** Both insert sites bind that
column from `event.entry.payload().bytes()` — the project's opaque payload and
nothing else. The same value is already spelled `opaque_project_payload` sixty
lines away, in the reducer envelope `reduceEnvelopeJcs` assembles in the same
file, so the file contained both spellings of one thing.

| Was | Is | The schema member it stores |
|---|---|---|
| `journal_events.canonical_event` | `opaque_project_payload` | `JournalEvent.opaque_project_payload` |
| `journal_events.canonical_provenance` | `provenance` | `JournalEvent.provenance` |

The second row goes beyond what was reported and is included for the same
reason: `provenance` is what the schema calls it and what the reducer envelope
in that file already calls it.

**The line this draws, so the next reader does not over-apply it.** Only a table
whose row IS a record takes that record's member names. `canonical_manifest`,
`canonical_observation` and `canonical_proposal` are untouched: those rows carry
a document as one blob beside columns that are not schema members, so
`canonical_` remains the right vocabulary there.

## 4. The envelopes are right; `project_state` in one is not a `ProjectState`

**Reported:** the reduce, derive, plan and step envelopes carry the opaque
payload where the schema defines an eight-member object, so either the schema
describes something that does not travel or the envelopes under-carry.

**Verdict: the consumer misread, and the first horn of its own dilemma is the
answer.** `$defs.ProjectState` describes the durable record; the envelope member
named `project_state` carries that record's `canonical_opaque_payload` and
nothing else, which is what a plugin's own reduce, plan and next_step schemas
are written against. Carrying the whole record would fold `revision` and
`last_journal_sequence` into the plugin's decision input, and the Operator
excludes every revision counter from decision inputs on purpose — see the
`DecisionBasisParts` comment in `ledger.cpp`: an identical world observed after a
takeover must not read as a different decision.

Nothing is changed for this point. What removes the ambiguity is §2 and §3:
`canonical_opaque_payload` is now the name of the bytes the envelopes carry, so
the correspondence is legible from either file.

## 5. Provenance is validated by the framework now, not by the deployment

**Verdict: already established, and closed here.**
[Consumer onboarding](2026-08-11-consumer-onboarding.md) §6.4 and
[checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) recorded that
`JournalProvenanceValidator` was delegated to the project while the schema it
answers for is framework-owned and fixed, that both fixtures implemented it as
one byte-comparison, and that both literals violated `$defs.JournalProvenance`.
The recommendation was to delete the callback. That is what happened.

- `JournalProvenanceValidator` is gone. `ProjectJournalSchemaOwner::create`
  takes one validator, and `validate()` enforces `$defs.JournalProvenance`
  itself.
- The check is a positional reader over exact RFC 8785 JCS. Because the bytes
  are canonical, the four members arrive in UTF-16 code-unit order — `kind`,
  `observation_ids`, `principal_id`, `source_hashes` — and reading them in that
  order is what enforces `required` and `additionalProperties: false` at the
  same time. A document ordered any other way is refused rather than reordered.
- Both fixtures now mint conforming documents, and they exercise different
  branches on purpose: umbraflow uses a null `principal_id` with a non-empty
  `observation_ids`, arcana a named principal with a non-empty `source_hashes`.

The registration-pin test from onboarding §6.2 is what settles the authority
question and is worth restating, because it is mechanical: no member of
`ProjectRegistrationClaims` pins a provenance schema, because nothing about that
shape is a project's to decide. A project supplies provenance *values*; it never
supplied the schema that judges them.

---

## 6. What is now a gate rather than a claim

`schema-state-s01` reads schema text, and its name says so — the repository's
`contract-`/`schema-` vocabulary is honest about that and the consumer's
complaint about it does not stand. `contract-state-s06` is a behavioural gate
and its name is right too: it exercises baseline immutability, refuses a session
pinned to an unprovisioned key, and drives the revision ABA. The consumer's
claim that its name is wrong is not supported.

What *was* missing is narrower and real: the record-shape half of both `S-06`
and `A-04` stood on schema text alone, and no assertion anywhere compared the
schema with the DDL. Four names could drift and stay green.

Two assertions now close that, in the requirements that own the records:

- `contract-state-s06` — the `project_state` column set equals
  `$defs.ProjectState`'s `required` list, both directions.
- `contract-agent-a04` — the `journal_events` column set equals
  `$defs.JournalEvent`'s `required` list, both directions.

They read the columns back from a database the Operator created, through
`tests/operator/schema-binding.hpp`. The coordinator holds its file under
`PRAGMA locking_mode=EXCLUSIVE` for its whole lifetime, so the helper opens a
runtime directory of its own, closes it, and reads the file the DDL left behind.
No source text can satisfy either assertion.

`contract-agent-a04` also now drives six provenance documents that each violate
exactly one rule of `$defs.JournalProvenance` — the `kind` enum, a missing
required member, a fifth member, the `Hash` pattern, `uniqueItems`, and the
`Identifier` pattern — plus the conforming document, which must be accepted. A
framework check that merely compared bytes against the conforming document would
pass the six; that is why the positive case is asserted beside them.

No gate was added or renamed. `ctest -N` reports 84 tests, 40 `CONTRACT` and 19
`SCHEMA`, before and after.

## 7. Falsification

Each mutation was built and run on its own, and the tree was restored and
re-verified green afterwards.

| Mutation | Expected | Observed |
|---|---|---|
| In the schema only, rename `ProjectState.project_state_schema_hash` to `state_schema_hash` | `contract-state-s06` red, nothing else | red — `contract-state-s06` and its aggregate, 82 of 84 passing |
| In the DDL only, rename `opaque_project_payload` back to `canonical_event`, with the fingerprint correctly recomputed so the store still opens | `contract-agent-a04` red, nothing else | red — `contract-agent-a04` and its aggregate, 82 of 84 passing |
| `validateJournalProvenance` returns `ok()` for every input | `contract-agent-a04` red on six assertions | red — exactly 6 failed assertions in `a04`, plus `test-operator` |

The first is the one worth keeping in mind: before this change, no edit to
`schema/umbraflow-journal-v1.schema.json` alone could turn a behavioural gate
red.

Assertion counts, per binary, baseline `ab97e2d` to now:

| Binary | Before | After |
|---|---|---|
| `test-contract-operator` | 4512 | 4624 |
| `test-operator` | 2454 | 2454 |
| `test-contract-runtime` | 612 | 612 |
| `conformance-umbraflow` | 1309 | 1309 |
| `conformance-arcana` | 1471 | 1471 |

The whole `+112` is inside the two cases that changed — `contract-state-s06`
went to 219 and `contract-agent-a04` to 228, and the other cases in that binary
total 4177 both before and after. The provenance edits touch the shared fixture
but add no assertion to it: the deleted validator lambda contained none, and the
literals that replaced `{"kind":"fixture"}` are data.

## 8. The fingerprint, and what it costs

`k_exactSchemaV1Fingerprint` moved to
`sha256:500c07b10eb263c0f2d6001e0a8b9a90ddd2afd951130cef71f5dbbfbd66085a`
over the same 23 tables — four columns renamed, no table added or dropped, so
`expectedTables` is unchanged for the same reason `07abc3e` left it unchanged.
It occurs exactly once in the tree, at
`modules/operator/source/operator/ledger.cpp:362`.

**An `operator-runtime.sqlite` created before this change is refused at open and
is deleted, never migrated.**

The recompute was positive-controlled first, against two earlier `ledger.cpp`
revisions rather than one: the recipe reproduced `bda31e4b18…` at `07abc3e~1`
and `be80aca714…` at `07abc3e`, each matching the constant that commit carries,
before it was trusted to produce a new value.

## 9. What this owes elsewhere

`CONTEXT.md` line 48 still carries `be80aca714…` as the Operator DDL
fingerprint. That file is outside this change's path ownership and needs the new
value, alongside `opaque_project_payload`, `provenance`,
`project_state_schema_hash` and `canonical_opaque_payload` as the one spelling
for each of those four columns.
