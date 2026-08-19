# 2026-08-19 — A document holds no fact that something else verifies

## Decision

A document in this repository must not hold a fact that another artifact can
verify. In particular, no document here states another repository's version,
status, or digest, and none transcribes a digest of bytes this repository
generates.

Documents are sorted into layers, and a document belongs to exactly one:

| layer | content | who maintains it | can it drift |
|---|---|---|---|
| L0 | code, `schema/`, tests | machines | no — it reds |
| L1 | status | the consumer's execution ledger, not this repository | not held here |
| L2 design | system shape, module ownership, deliberate absences | humans, editable | low |
| L2 decisions | dated rulings in `docs/decisions/`, frozen | written once | not applicable |
| L3 index | pointers only, no facts | rare edits | low |

On finding drift, the first question is not "how do I correct this sentence" but
"should this sentence be here at all". Usually the answer is no, and deleting is
cheaper than fixing. Amendments are no longer appended in place: the ruling goes
to `docs/decisions/` and the live document is edited to be simply true.

## Context

`docs/ARCHITECTURE.md` proved this from its own history. It transcribed the
consumer bundle's root hash five times in five days and was wrong on four of
them — v1.9 stale through v1.10, v1.11 and v1.12, then a v1.13 correction, then
a v1.18 correction, each one a hand-maintained copy of a number owned somewhere
else. The file eventually *said so in a blockquote* and kept transcribing
anyway: measured on 2026-08-19 it was 249 lines, of which 69 were amendment
blockquotes and 11 were `Amended`/`Corrected` notes, and line 17 still claimed
contract version v1.18 while the consumer's interface lock had moved to v1.19.
`docs/INDEX.md` repeated the same stale claim.

The root cause is not carelessness. A document that holds a fact someone else
owns can only be kept true by hand, forever, and the amendment convention made
that labour look like diligence: every correction added lines, and the added
lines were themselves facts to maintain. The correction rate was the argument
against the practice, not evidence that the practice was working.

The removal of the byte pin
([2026-08-16](2026-08-16-no-exact-byte-consumer-bundle-pin.md)) took away the
mechanism; this ruling takes away the habit.

## Consequences

- `docs/ARCHITECTURE.md` is pure L2 design: module ownership, dependency
  direction, boundaries, and the deliberate absences. It states no status and no
  version belonging to the consumer, and it carries no amendment blockquotes.
  Deliberate absences are its most valuable content, because nothing in the code
  records a decision *not* to build something.
- `docs/INDEX.md` is L3: pointers only. It holds no version, no status and no
  digest. It keeps two symmetric rules — this repository does not copy the
  consumer's status rows, and it does not reference the consumer's internal
  documents either. What the consumer needs from us is in the generated
  [`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).
- `docs/decisions/` holds the frozen rulings that were previously spread across
  amendment blockquotes. See [its README](README.md) for why this is not a
  return of the retired `docs/adr/`.
- `.claude/skills/correct-doc-drift/SKILL.md` carries the new premise. Its
  drift-class table, blast-radius sweep, one-decision-one-sweep rule and
  pre-archive rule survive unchanged; its "amend in place with a dated note"
  correction rule does not.
