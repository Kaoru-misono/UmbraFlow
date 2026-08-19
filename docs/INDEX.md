# Documentation Index

Pointers only. This file holds no version, no status, no digest and no measured
count: those are facts something else owns and verifies, and a copy of one here
could only be kept true by hand.

## Contract and design

- [`schema/`](../schema/) — the normative document shapes. The files are the
  authority; no prose here restates them.
- [`docs/design/`](design/) — what things currently *are*. The
  [runtime model contract](design/2026-08-09-runtime-model-contract.md) explains
  `schema/umbraflow-runtime-v3.schema.json`.
- [Architecture](ARCHITECTURE.md) — module ownership, dependency direction, and
  the deliberate absences.
- [Domain glossary](../CONTEXT.md) — terminology authority.

## Decisions

- [`docs/decisions/`](decisions/README.md) — dated rulings, one per file, frozen.
  A changed mind writes a new file; the old one keeps its bytes. Read
  [the README](decisions/README.md) for why this is not the retired `docs/adr/`.

## Plans

- [`docs/plans/`](plans/README.md) — live work and product direction. A plan
  holds work, not rulings; a ruling that turns up inside one moves to
  `docs/decisions/`.

## Published outward

- [`docs/PUBLIC-CONTRACT.md`](PUBLIC-CONTRACT.md) — generated, and the only
  document a consuming repository reads.

## Two rules about the consumer repository

Neither direction of reference is allowed, and the symmetry is the point.

- This repository does **not** copy the consumer's status, versions, blockers or
  acceptance criteria. That ledger is the consumer's; a copy here is stale from
  the moment work continues, and it drifts silently because nothing here can
  check it.
- This repository does **not** reference the consumer's internal documents
  either — not by path, not by section. What the consumer needs from us is in
  `docs/PUBLIC-CONTRACT.md`, which is generated from bytes in this repository.

## Before investigating a failure

Read [Pitfalls](pitfalls/README.md), beginning with
[checks that cannot fail](pitfalls/checks-that-cannot-fail.md). A green name is
not evidence unless its failure mode was observed.

## History

[`archive/plans/`](archive/plans/) and [`archive/reviews/`](archive/reviews/)
retain completed, superseded and measurement-only documents, frozen. Nothing
there is current, and nothing there is edited.
