# 2026-08-22 — Production reachability is the cut invariant

## Decision

A replacement generation is built bottom-up as complete, compiled,
conformance-tested but **production-unreachable** components across many
commits. One terminal commit rewires the production entry to the new
generation, migrates every consumer, and deletes the superseded paths in the
same change.

The invariant that separates a legitimate intermediate state from the forbidden
compatibility path is production reachability, not coexistence in the tree:

- A **forbidden compatibility path** is two generations simultaneously reachable
  from the production entry, with a selector choosing between them.
- A **legitimate intermediate state** is new-generation code reachable only from
  tests.

Two rules are checkable per commit. Exactly one generation is
production-reachable, and the gate passes. And no commit ever introduces a
selector — no flag, no version field, no dual entry table, no "if the new
catalog is present" branch, not even temporarily. The moment a runtime value
chooses a generation, the forbidden thing exists regardless of the intent to
remove it.

Tests referencing dark code are not a dual entry table. The rule forbids a
mixed-generation runtime; tests are not the production runtime.

Nothing is released, so there are no durable bytes to migrate at the flip. Where
development-time ledgers exist, they are regenerated rather than taught to read
two row shapes.

## Context

`docs/decisions/2026-08-21-tools-are-the-shared-game-driving-boundary.md`
requires one atomic replacement and states that it "creates no
mixed-generation runtime". Read literally as a property of the working tree,
that forbids the only way a cut of this size can actually be built: the new
identity rows, catalogs, program type, providers, adapters and scaffolds cannot
land in one commit, and each of them is meaningless until the ones beneath it
exist. Every staged commit would then be a formal violation of the ruling it is
executing.

The tension is not real, because the harm the ruling names is a *runtime*
harm — a caller reaching two generations, a reader unable to tell which path
carries the contract, a fallback that outlives its reason. None of those exist
while the new generation has no production caller. What must never exist is the
thing that creates them: a runtime value that chooses. A selector is the
observable signature of a compatibility path, and it is checkable per commit in
a way that "is this staged or is this a bridge?" is not.

This is also why "delete the old path in the same commit that makes the new one
reachable" is the flip rule rather than a later cleanup. A cleanup deferred by
one commit is a commit in which both generations are reachable.

## Consequences

- A commit may add a complete, tested subsystem with no caller. Reviewers must
  not read an absent production caller as unfinished work or as drift.
- A plan describing such a cut must say which generation is production-reachable
  at the commit it describes. A present-tense "there is no dual path" sentence
  is only true of the finished state and must be written as the constraint on
  the flip, not as a description of the tree.
- A "temporary" feature flag, environment variable, catalog-presence branch or
  dual entry table is refused at review even when the same change promises to
  delete it.
- Dark code needs conformance coverage before the flip, because the flip commit
  is not the place to discover that the new generation is wrong.
- The dependency order for a cut is bottom-up: identity, envelope and ledger row
  types plus their conformance; then catalogs and validation; then the scoped
  program type, resolver and facades tested against a fake executor; then
  providers and the observation and input contracts; then registration and
  manifest pinning with policy, budget, lease and recovery; then adapters and
  scaffolds; then the flip.
