# 2026-08-17 — A public method with only test callers is a finding, not a verdict

## Decision

An Operator public method whose only callers are in `tests/` is a **candidate**,
graded into tiers before anything is concluded from it. Three tiers, and only the
first is a confirmed defect:

- **Confirmed bypass** — production does the same job by another path that omits
  the checks. The guard exists, the negative cases exist, and real data goes past
  them.
- **Confirmed unwired** — the capability is implemented and correct, and nothing
  in production can trigger it. No false path exists; it is merely out of reach.
- **Excluded** — either by design or on evidence. A method may exist *to be read
  by tests*, and two methods that look interchangeable may not be.

An exclusion must be argued from evidence, not from absence. **A method having no
callers is never itself proof that a requirement is unmet.**

The scan is reproducible and depends on no uncommitted tooling: take the public
method names from `modules/operator/source/operator/ledger.hpp`, search
`modules/`, `entry/` and `tools/` for `\b<name>\s*\(` excluding the declaration
and definition, and every name landing only in `tests/` is a candidate.

## Context

The scan was run after the observed-instance minting boundary turned out to have
no production entry point (see
[2026-08-17 — observed-instance minting owners](2026-08-17-observed-instance-minting-owners.md)),
to find out whether that was an isolated case. It was not: of 37 public
`OperatorCoordinator` methods, 12 had callers only in tests, while production's
`modules/service/source/service/product-lifecycle.cpp` called seven.

Grading mattered more than counting, and two exclusions show why the tiers are
not bureaucracy.

`remainingBudget` is **excluded by design**. Its declaration says it is the sole
reader of the stored counter, so that a case asserting a decrement happened reads
the database rather than a number computed by the same call. Having only test
callers is its purpose.

`resumeSession` is **excluded on evidence**, and this is the exclusion that would
have caused a wrong verdict. `resumeSession` restores a session *identity* by
reactivating the most recent matching historical session.
`recoveredUncertainDispatches` reports the pending Operations that a restart
handed to reconciliation, and that query is durably repeatable so a crashing
caller cannot consume the only copy. Production uses the second, and genuinely
consumes its result — `product-lifecycle.cpp` passes it to a function returning
writable when the recovery set is empty and read-only otherwise, which is the
literal implementation of the restart-recovery acceptance clause. Concluding that
the requirement was unmet because `resumeSession` had no callers would have been
wrong on a correct implementation.

The scan's own finding about itself is the reason this is a ruling rather than a
recorded result: it was needed at all because **"has a falsifiable guard" was
being read as "this property holds in production"**, and those are different
claims.

## Consequences

- The scan becomes a recurring check rather than a one-off investigation. Filed
  as a historical result it is lost the moment the code moves; the obligation to
  build it is in
  [the live gap ledger](../plans/2026-08-19-framework-verification-gaps.md).
- A finding is written with its tier and its evidence, never as a bare count.
- Reader methods without production callers are a different and milder question
  than mutators, and were not expanded.
- This is the same failure the general documentation rule later generalized:
  [2026-08-19 — a document holds no fact that something else verifies](2026-08-19-documents-hold-no-foreign-facts.md).
