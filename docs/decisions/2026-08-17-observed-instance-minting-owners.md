# 2026-08-17 — Who owns observed-instance identity and world scope

## Decision

**`ObservedInstanceIdentitySchemas` belongs to registration.** The registration
owns a closed identity-schema set, and the deployment loader supplies and
compiles its exact documents. This is a wire change: the deployment source gains
a required member naming the schema file paths, the registration gains a required
member carrying their sorted, deduplicated lowercase sha256 digests, and
`project_registration_format` moves from 1 to 2. A consumer's interface lock must
move with it, and must freeze the registration schema's byte copy together with
its positive and negative closure vectors.

**`ObservedInstanceWorldScope` belongs to the pinned session.** `ProductStart` is
its production acquisition edge: `ProductStart` and `SessionPin` carry a required
world scope, the session persists three immutable columns
(`world_scope_kind`, `world_scope_id`, `world_scope_generation`), and
`createSnapshot` re-reads those three inside its own transaction. This changes
neither the proposal nor the registration wire.

**Scope is constructed, never inferred.** An account scope must be built from an
authoritative binding; a zero value is permitted only when explicitly given.
Rows predating the scope columns migrate to sentinel values, and an observation
whose scope cannot be established is refused rather than guessed.

## Context

The ruling was forced by a measurement, and the measurement is the reason this
file exists rather than a status row.

The minting boundary had been delivered, reviewed and gated — and had no
production entry point. `mintObservedInstanceBinding` was reachable only from
`OperatorCoordinator::publishProjectObservation`, and every caller of that method
was in `tests/operator/test-ledger.cpp`; `modules/` contained none. Production
observe took a different path: the plugin's derived document was checked for its
registration hash, function and direction, then written straight to
`project_observations`. No proposal was constructed, nothing was minted, and no
collision, scope or freshness check ran.

This is the most dangerous shape a defect can take. The guard existed, the
negative cases existed, and they would go red if broken — while the real data
went past them. A green suite proved a path production did not take.

The disposition was deliberately left unruled at the time of measurement, because
two readings were available and only one could be right: route observe through
the minting boundary, or accept the direct path as legitimate and narrow the
delivered scope to match. This file is the ruling that closed that choice, and it
chose to route.

## Consequences

- `createSnapshot` synthesises the proposal from the validated derived document
  through `proposalFromDerived`, which fails closed as `MalformedProposal` on any
  missing or mistyped member, and mints through the same canonical implementation
  `publishProjectObservation` uses. The bytes stored are the final observation's,
  not the plugin's derived bytes. The identity authority is the one the loaded
  deployment carries, never one read out of plugin output.
- Production entry points gate identifier resolution: `submitCommand` resolves
  every observed-instance id in a command's canonical arguments — exact world
  scope plus fresh membership — before any read-only operation can complete or a
  plan may consume the id, and `mintNextStep` guards a step's target identifier
  again.
- The second persistence chain is gone. The observe path and the publish path are
  one canonical mint.
- Review before landing caught three blockers — a misplaced resolution, a missing
  target-binding join, and a derived envelope that could terminate the
  operator — each closed with a falsifiable test.
- The proposal envelope, the final observation envelope and the precondition
  fragment are published framework schemas. The two public spellings of the
  offered-tool accessor were deleted; the snapshot member is the sole public
  exit, per "break it rather than bridge it".
- One duplication is recorded and knowingly not yet closed — see
  [the live gap ledger](../plans/2026-08-19-framework-verification-gaps.md).
