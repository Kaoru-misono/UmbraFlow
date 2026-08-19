# 2026-08-15 — `journal_envelope_schema_hash` leaves `SessionManifest`

## Decision

`journal_envelope_schema_hash` is removed from `SessionManifest`. This was Stage
H5 of the framework hash cleanup.

## Context

The field was written and serialized, and had no accessor, no reader and no
refusal anywhere. A hash that nothing compares is not a proof — it is a value
that participates in an identity while being unable to reject anything, so it
makes every manifest root move when the schema file is cosmetically edited and
catches nothing in exchange.

That is the general admission rule the hash cleanup applied: a digest earns its
place only where an automatically produced content identity sits at a real
immutable-byte boundary and something refuses on mismatch.

## Consequences

- `SessionManifest` has one fewer field; there is no compatibility reading that
  accepts the old shape, and no "absent means the old behaviour".
- The reasoning and the other four stages are in the archived
  [framework hash cleanup](../archive/plans/2026-08-14-framework-hash-cleanup.md).
