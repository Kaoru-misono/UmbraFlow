---
name: correct-doc-drift
description: Correct factual, decision, and terminology drift in project documents (plans, TODO, CONTEXT.md, S0 contracts, knowledge base). Use when a decision is finalized or changed, when a document contradicts the code or a newer document, when it still calls something deferred/blocked/待定 that has since been resolved, or when one document queues a correction to another.
---

# Correct Document Drift

Documents in this repo are load-bearing: plans gate implementation, the S0
contract gates schemas, CONTEXT.md gates naming. A sentence that was true when
written and is false now will be trusted by a future session. This skill fixes
drift at the moment it becomes visible, not in a someday cleanup.

## Drift classes (all real incidents from this repo)

| Class | Example |
|---|---|
| **Superseded decision** | Script root ruled `uf` (2026-07-29); older docs still teach `umbra.*`, older ones still `bot.*` |
| **Stale blocker wording** | integration-plan said S0 schema "deferred" 4 days after it locked |
| **Queued-but-unapplied fix** | hardening ledger ordered a veto #2 rewording in the roadmap; nobody applied it |
| **Doc-vs-code contradiction** | "the engine has no loop" vs `waitForPage`'s internal poll loop |
| **Terminology drift** | one concept spelled `bot`/`umbra`/`uf` across three documents |

## When to run

1. **Decision landing** — a grill, ADR, or plan approval changes a ruling:
   sweep the blast radius in the same session.
2. **Contradiction on contact** — any doc read during unrelated work
   contradicts code or a newer doc: fix small drift inline now; never
   silently move on past a known-wrong sentence.
3. **Pre-reliance** — before building on a doc's load-bearing claim
   ("X is blocked", "Y is the schema"), verify it against code/newer docs.
4. **Queued fix discovered** — a doc orders a correction elsewhere: apply it
   now. Queued fixes rot (see the veto #2 incident).

## Process

1. **Verify the drift is real.** The authority chain decides which side is
   wrong: roadmap = product direction, annotation-design = S0 contract,
   grill-decisions = task semantics deposit, CONTEXT.md = terminology,
   code = implementation truth. Read the authoritative side before editing
   anything — the "contradiction" may be your misreading.
2. **If code is the drifted side, this is not a doc fix.** File
   `TODO(cpp-debt)` or a pitfall entry instead; never rewrite a correct
   document to match wrong code.
3. **Sweep the blast radius.** Grep the stale term/claim across `docs/`,
   `CLAUDE.md`, `CONTEXT.md`, and `.claude/skills/`. List every hit before
   editing any.
4. **Correct each hit by document type** (table below). Small fixes land now;
   structural rework (restructuring TODO sections, full knowledge resync)
   gets a marker plus a note recording exactly what remains.
5. **Report**: what changed, what was deliberately left as history, what was
   deferred and where that is recorded.

## Correction rules by document type

| Document type | Rule |
|---|---|
| LOCKED / developer-approved docs (annotation-design) | Never silently rewrite. Amend inline **only** for developer-approved changes, with a dated note: `> Amended YYYY-MM-DD: ...` |
| Decision logs (grill-decisions) | History is immutable. Add a dated redirect note at the top ("read X as Y, see <artifact>"); never rewrite past rulings |
| Current-authority prose (roadmap constraints, plan bodies) | Fix inline, append a dated parenthetical naming the deciding artifact |
| Execution lists (TODO.md) | Fix stale claims, add a pointer to the deciding artifact; checkbox restructuring is a separate pass, not this one |
| Knowledge base (docs/knowledge/) | Small factual fix: edit **both** language mirrors. Larger drift: add the repo's DIRTY banner to both mirrors — `> **DIRTY (YYYY-MM-DD)**: <what outdated it>. Trust the code and <plan> until resynced.` — then regenerate later (atlas flow) |
| CONTEXT.md | Renames go here **first**: new canonical term, old term into `_Avoid_`; then propagate outward |

## Rules

- Every correction is dated and links the deciding artifact (ADR, plan,
  commit, or grill session note). Undated corrections are future drift.
- Never delete history — amend, annotate, strikethrough, or redirect.
- One decision, one sweep: fixing the doc you happened to be reading while
  leaving sibling hits stale converts one drift into two.
- Chinese/English mirrored docs are one unit; a fix that touches only one
  language is half a fix.
- This repository retired ADRs (82f8027). Decisions live in `docs/plans/` and
  terminology in `CONTEXT.md`. Do not reintroduce `docs/adr/`, and treat a
  document that still points at it as drift.
