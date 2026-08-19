# 2026-08-09 — Consumer examples never become core contracts

## Decision

The upstream core implements no game entity: no consumer tool names, no
consumer session type, no consumer Journal event names, no consumer content
states. The examples inside the consumer's design documents are non-normative for
upstream source.

Generic structs repeated in a consumer document do not create a second wire
schema. The checked-in upstream schemas under `schema/` are the sole executable
shape, and consumer payloads remain schema-validated opaque data.

Two structurally different upstream fixture plugins are the local framework gate
only. They do **not** satisfy the real dual-game gate.

## Context

One of the four executable specification resolutions, following the consumer main
design §6 explicit project-ownership boundary. The failure mode it forecloses is
cheap and quiet: a consumer document illustrates a generic mechanism with a
concrete game example, someone implements the example because it is the concrete
thing on the page, and the framework acquires a branch named after one game that
the next consumer must satisfy without knowing why.

The same reasoning applies to structs: a document that shows a generic struct
inline has not published a schema, and treating the illustration as normative
creates a second spelling of a shape whose real definition is a checked-in file.

## Consequences

- No consumer-specific branch exists in Host, Runtime or Operator.
- Game semantics and payload schemas are owned by the external ProjectPlugin
  consumer.
- The real dual-game attestation must run the same conformance suite against two
  real, independently owned registrations, record both exact
  `project_registration_hash` values, and pass before either consumer opens
  production mutation. It is external, cannot be made green by a fixture, and is
  deliberately not claimed by this repository.
