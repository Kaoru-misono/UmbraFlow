# 2026-08-13 — One generated runtime catalog for the published schemas

## Decision

`modules/schema` is a leaf module whose framework schema catalog is generated
from the published files under `schema/`. Every module that needs a published
schema at runtime depends on that one catalog. No module owns a second schema
spelling.

## Context

The published files under `schema/` are the normative shapes, but a running
module cannot read a repository directory: it needs the bytes compiled in. Left
to itself each consumer of a schema grows its own copy of the relevant subset,
and the copies diverge from the published file silently — the published file is
edited, nothing reds, and the weaker in-module reading keeps validating
documents the schema no longer accepts.

Generating one catalog from the published files makes the published file the only
authorable thing, and makes divergence impossible rather than merely discouraged.
`modules/schema` depends on nothing so that any module may depend on it.

## Consequences

- Deployment, Operator and Project depend on the one runtime catalog.
- Editing a file under `schema/` is the only way to change what any module
  validates against; there is no second place to edit.
- The catalog is generated, so it holds no hand-written digest and is not a
  document that can drift.
