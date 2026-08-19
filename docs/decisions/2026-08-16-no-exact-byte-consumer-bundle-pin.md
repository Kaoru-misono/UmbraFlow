# 2026-08-16 — The consumer bundle carries no exact-byte pin here

## Decision

The exact-byte root pin on the consumer's specification bundle is removed from
this repository. The contract version is semantic: two repositories agree when
they implement the same contract version. This repository states no digest, no
root hash, and no version number for that bundle in any of its documents; where
a document needs to talk about the contract it names the consumer's own
interface lock as the authority and stops there.

This executed Stage 1 of the hash management simplification proposal.

## Context

Three measurements decided it, and each is checkable rather than a matter of
taste.

**It was a CI-labelled test that depended on another repository's absolute
path.** `check-spec-bundle` carried `LABELS "CI"` in `tests/CMakeLists.txt`, so
it ran inside `ctest -L CI` and therefore inside `GATE: PASS` — the sentence
claiming it stayed out of `ci-local` was itself wrong, and the archived
hash-cleanup plan had already caught that. It located the bundle by
regex-matching an absolute path out of prose. On any machine without that
checkout, this repository's own gate could not pass. A reusable project
foundation cannot have that.

**Half of it never fired.** The digest printed in the document was only ever
used to locate the checkout; the value actually compared was the constant inside
the script. Mutating the document's root to a different digest left the gate
green and printing `VERIFIED`. That was a live divergence, and it disappears
with the pin rather than needing a repair.

**What it guarded is guarded better elsewhere.** The interface lock's normative
content — schemas and vectors — is byte-pinned by the consumer's own
interface-lock manifest and verified by the consumer's own automated suite,
including a refusal of CRLF. Pinning the Markdown on top of that was a second
spelling of one thing. The remaining bundle documents are design prose, and nine
known textual divergences sat inside them: every one was blocked from repair by
the pin, and repairing prose was never what byte equality was for.

## Consequences

- No gate registered by this repository may REQUIRE another repository. The note
  at `tests/CMakeLists.txt` beside the surviving checks records that rule and
  names this ruling as its source.
- The interface-lock parity gate that took over the part of this job worth doing
  is ruled separately, on the following day — see
  [2026-08-17](2026-08-17-interface-lock-parity-gate.md).
- Residual risk, stated rather than hidden: outside what the interface-lock
  vectors cover there is no mechanical check that both repositories read the
  same contract version. Closing that is a review obligation on the version
  number, owed by whoever next revises the lock — as is the coverage question of
  which of the lock's prose conventions must be mechanically held and which are
  left to review.
- The five transcriptions of the bundle root that this repository's
  `ARCHITECTURE.md` carried between 2026-08-12 and 2026-08-16 — four of them
  wrong — are the evidence behind
  [2026-08-19 — documents do not hold other repositories' facts](2026-08-19-documents-hold-no-foreign-facts.md).
