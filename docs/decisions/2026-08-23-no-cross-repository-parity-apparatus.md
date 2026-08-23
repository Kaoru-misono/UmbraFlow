# 2026-08-23 — The interface-lock parity gate is deleted, and its category with it

This supersedes
[`2026-08-17-interface-lock-parity-gate.md`](2026-08-17-interface-lock-parity-gate.md),
which registered `check-interface-lock-parity`. That gate, its script
`tests/contracts/test_interface_lock_parity.py`, and the
`UF_CONSUMER_INTERFACE_LOCK` cache entry are removed.

## Decision

This repository builds no cross-repository parity apparatus. No interface lock,
no frozen expectation vectors, no producer-against-consumer conformance suite,
and no variant in which one repository imports another's recorded expectations in
order to detect that it has broken them.

The owner ruled the category out, not the instance.

## Why the instance deserved it anyway

**It never ran once.** The gate was registered in a skipped state whenever no
consumer lock was declared, and no consumer ever published one. `uf-chaos`, the
only consumer, has no `conformance/` directory at all. So from the day it was
registered it reported itself by name as not run, on every gate, forever — which
is this repository's own "a check that exists and cannot fail", the defect the
2026-08-17 decision was itself written to cure.

**Lit, it would still have missed the break it existed for.** Its own decision
recorded the gap in its final line: contract-version agreement between the two
repositories remained a review obligation outside what the lock's vectors cover.
On 2026-08-23 the shape of the closed deployment object changed while the schema
name stayed `umbraflow-project/v2`, and the consumer was refused at its
declaration. That is exactly the class the gate did not cover, handed to a
"review obligation" with no owner and no trigger, which accordingly nobody
performed.

So the apparatus was both dark and aimed elsewhere. Restoring it would have
meant building the consumer half, then extending the vectors to the thing it
admitted it did not cover — inventing a second machine to watch the first.

## What takes its place

Nothing that watches. The answers this repository funds instead are the ones
that stop the break reaching a consumer silently rather than detecting it
afterwards:

- A release note that states what BROKE and what it was BEFORE, so the author of
  an old declaration reads the transition rather than discovering it.
- `project upgrade`, which installs the new binaries without destroying the old
  and then reports the complete adaptation list from the newly installed
  binary's own schema.
- One shape authority, compiled by every reader, so no reader can accept what
  another refuses.

A gate suite measures a tree's self-consistency, and a breaking change that
updates every internal caller in the same commit — which this repository's own
rules demand — preserves self-consistency perfectly. Ninety-one gates were green
while the only consumer was broken. Adding a ninety-second is the reflex that
produced the first ninety-one.
