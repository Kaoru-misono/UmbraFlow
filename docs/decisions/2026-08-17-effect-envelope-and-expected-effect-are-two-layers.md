# 2026-08-17 — `EffectEnvelope` and `ExpectedEffect` are two layers, not two spellings

## Decision

`EffectEnvelope` is the Operator's effect wire envelope: a namespaced type, risk,
scope and payload schema hash. `ExpectedEffect` is a project-domain concept — what
may or should happen — and is the pole that a project's `ObservedOutcome` is
contrasted with.

They are not a rename, not a duplicate, and must not be unified. A framework
document naming `ExpectedEffect` as an Operator record is wrong; a project
document naming `EffectEnvelope` as a domain concept is equally wrong.

## Context

The pair looked like classic terminology drift — one concept spelled two ways
across two repositories — which would have called for picking one and deleting the
other under "break it rather than bridge it". A census of the specification
documents found the opposite: each name is correct in its own layer, and the
apparent conflict came from documents on one side naming the other side's record.

Merging them would have destroyed a distinction the requirement matrix explicitly
depends on, since the requirement in question exists precisely to forbid merging
the expected pole with the observed one.

## Consequences

- The published Operator schema and `modules/operator/source/operator/` use
  `EffectEnvelope`; `modules/` defines no `ExpectedEffect`, and should not.
- Two framework-side inconsistencies were identified and are owned by
  [the live gap ledger](../plans/2026-08-19-framework-verification-gaps.md): a
  local `EffectEnvelope` helper in `effective-plan.cpp` versus the published
  schema, and this repository's own offline/online Agent terminology.
- Terminology drift and layer separation look identical from one document. The
  check is whether each name has a distinct referent, not whether two names
  appear.
