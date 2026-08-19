---
name: correct-doc-drift
description: Correct factual, decision, and terminology drift in project documents (plans, CONTEXT.md, design documents, knowledge base). Use when a decision is finalized or changed, when a document contradicts the code or a newer document, when it still calls something deferred/blocked/待定 that has since been resolved, or when one document queues a correction to another.
---

# Correct Document Drift

## The premise

**A document must not hold a fact that something else can verify.**

The first question on finding drift is not "how do I correct this sentence" but
**"should this sentence be here at all"** — and usually the answer is no, and
deleting is cheaper than fixing.

This premise replaces the older one, which held that drift is inevitable and the
duty is prompt manual correction with a dated amendment appended in place. That
premise was tested to destruction. `docs/ARCHITECTURE.md` transcribed the
consumer bundle's root five times in five days and was wrong on four of them,
each correction faithfully appended as a dated note; by 2026-08-19 it was 249
lines, of which 69 were amendment blockquotes and 11 were `Amended`/`Corrected`
notes — and it was still wrong, one contract version behind. Every correction
added lines, and the added lines were themselves facts to maintain. The file
diagnosed its own disease in a blockquote and kept transcribing. See
`docs/decisions/2026-08-19-documents-hold-no-foreign-facts.md`.

Diligence was never the missing ingredient. A document that holds a fact someone
else owns can only be kept true by hand, forever.

## Layers: which document may hold what

| layer | content | who maintains it | can it drift |
|---|---|---|---|
| L0 | code, `schema/`, tests | machines | no — it reds |
| L1 | status | the consumer's execution ledger, not this repository | not held here |
| L2 design | system shape, module ownership, deliberate absences | humans, editable | low |
| L2 decisions | dated rulings in `docs/decisions/`, frozen | written once | not applicable |
| L3 index | pointers only, no facts | rare edits | low |

A document belongs to exactly one layer. Drift is almost always a document
holding content from a layer below it: a design document holding status, an
index holding a version, a plan holding a ruling.

**Deliberate absences are the exception worth protecting.** Nothing in the code
records a decision *not* to build something, so a list of what this repository
consciously does not have cannot be derived from bytes and must be written by
hand. That is `ARCHITECTURE.md`'s most valuable content.

## Drift classes (all real incidents from this repo)

| Class | Example |
|---|---|
| **Transcribed foreign fact** | `ARCHITECTURE.md` carried the consumer bundle's contract version and root hash; five transcriptions in five days, four wrong |
| **Transcribed derivable count** | the migration report stated gate totals that `tests/CMakeLists.txt` owns; six commits in a row updated the registrations and left the report behind |
| **Unsourced quantity** | "thirteen artifact families" appeared in prose with no source in either repository; `git log --all -S` found zero commits introducing it, and every candidate list was a different number |
| **Superseded decision** | Script root ruled `uf` (2026-07-29); older docs still teach `umbra.*`, older ones still `bot.*` |
| **Stale blocker wording** | integration-plan said S0 schema "deferred" 4 days after it locked |
| **Queued-but-unapplied fix** | hardening ledger ordered a veto #2 rewording in the roadmap; nobody applied it |
| **Ruling archived unexecuted** | the 2026-07-25 sweep ruled four `core` facilities through `evaluate-core-capability`; the review was archived on 2026-08-01 (`eb1d205`) and the ruling left the live set with it, still unrun 17 days later |
| **Doc-vs-code contradiction** | "the engine has no loop" vs `waitForPage`'s internal poll loop |
| **Terminology drift** | one concept spelled `bot`/`umbra`/`uf` across three documents |
| **Inherited self-description** | a claim copied from a commit message and recorded as measured; the commit said "no callers exist" and a caller existed |

The first three classes and the last are not fixed by correcting the sentence.
They are fixed by deleting it and, if a reader genuinely needs the fact, pointing
at whatever verifies it.

## When to run

1. **Decision landing** — a plan approval or review changes a ruling: sweep the
   blast radius in the same session, and write the ruling to
   `docs/decisions/`.
2. **Contradiction on contact** — any doc read during unrelated work contradicts
   code or a newer doc: never silently move on past a known-wrong sentence.
3. **Pre-reliance** — before building on a doc's load-bearing claim ("X is
   blocked", "Y is the schema"), verify it against code and newer docs.
4. **Queued fix discovered** — a doc orders a correction elsewhere: apply it now.
   Queued fixes rot.
5. **Pre-archive** — a plan or review leaving the live set takes its unexecuted
   rulings with it. Move each ruling to a live owner — a frozen ruling to
   `docs/decisions/`, an owed work item to a live plan — and name that owner in
   the file before it moves. **Nothing may be archived while something it owes
   lives only inside it.**

## Process

1. **Verify the drift is real.** The authority chain decides which side is
   wrong: `schema/` and the registrations in `tests/CMakeLists.txt` are
   machine-checked and win over prose; `CONTEXT.md` owns terminology; code is
   implementation truth; the consumer's own ledger owns status. Read the
   authoritative side before editing anything — the "contradiction" may be your
   misreading. Two `A-07` reversals in one day came from not doing this.
2. **Ask whether the sentence belongs at all.** If something else verifies the
   fact, delete the sentence and point at the verifier. Do not correct it.
3. **If code is the drifted side, this is not a doc fix.** File
   `TODO(cpp-debt)` or a pitfall entry instead; never rewrite a correct document
   to match wrong code.
4. **Sweep the blast radius.** Grep the stale term or claim across `docs/`,
   `CLAUDE.md`, `CONTEXT.md`, and `.claude/skills/`. **List every hit before
   editing any.**
5. **Correct each hit by document type** (table below).
6. **Report**: what was deleted, what was moved to `docs/decisions/`, what was
   deliberately left as history, and what was deferred and where that is
   recorded.

## Correction rules by document type

**Amendments are no longer appended in place.** A ruling goes to
`docs/decisions/` as its own dated file, and the live document is edited to be
simply true. A document is not a changelog; Git is.

| Document type | Rule |
|---|---|
| `docs/decisions/` | **Immutable.** Never edit. A changed mind writes a NEW dated file that names the one it supersedes; the old file keeps its bytes |
| `docs/ARCHITECTURE.md`, `docs/design/` | Edit to be simply true. No dated notes, no blockquote history. If the fact is derivable, delete it instead of correcting it |
| `docs/INDEX.md` | Pointers only. Any version, status, digest or count appearing here is drift by construction — delete it |
| Plan bodies (`docs/plans/`) | Fix inline. If the drifted sentence is a ruling rather than work, move it to `docs/decisions/` and leave a link |
| Archived documents (`docs/archive/`) | Frozen. Never edit, never correct — including their checkboxes. If an archived doc misleads, fix the live document that points at it |
| Knowledge base | Small factual fix: edit **both** language mirrors. Larger drift: add the DIRTY banner to both mirrors — `> **DIRTY (YYYY-MM-DD)**: <what outdated it>. Trust the code and <plan> until resynced.` |
| CONTEXT.md | Renames go here **first**: new canonical term, old term into `_Avoid_`; then propagate outward |

## Rules

- **Delete before you correct.** Ask what verifies the fact; if something does,
  the sentence is the bug.
- **One decision, one sweep.** Fixing the doc you happened to be reading while
  leaving sibling hits stale converts one drift into two.
- **Never destroy reasoning.** Deleting a transcribed fact is right; deleting the
  argument that produced a ruling is not. The ruling moves to
  `docs/decisions/` with its context and consequences intact.
- **A measurement is a fact about a date, and belongs in a dated file.** A
  measurement written into a live document becomes a claim about the present the
  moment the code moves. If the property must keep holding, the answer is a
  recurring check, not a sentence — "has a falsifiable guard" is not "this
  property holds in production", and a scan filed as a historical result is lost.
- **Never record another document's self-description as measured.** A commit
  message, a review summary, or a plan row saying "there are no callers" is a
  claim to verify, not evidence to copy. That exact claim was copied once and was
  false.
- **A rule enforced by nothing will be broken.** If the same drift recurs, the
  answer is a check that reads the document, not a stricter reading of the rule.
  Six consecutive commits inverted one ordering rule before this was accepted.
- Chinese/English mirrored docs are one unit; a fix that touches only one
  language is half a fix.
- **`docs/decisions/` is not a return of ADRs.** This repository retired ADRs in
  `82f8027`; do not reintroduce `docs/adr/`, and treat a document that still
  points at it as drift. ADRs were retired in favour of decisions living in
  `docs/plans/`. What `docs/decisions/` separates is the *frozen ruling* from the
  *live plan* — because a plan that also holds rulings is rewritten whenever its
  status changes, dragging the rulings along with every rewrite. Plans keep the
  work; decisions keep the reasoning; neither keeps the other's content.
