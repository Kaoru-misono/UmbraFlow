# 2026-08-19 - Operator protocol identities are framework-owned

## Decision

An embedded schema identity below `https://umbraflow.dev/schema/operator/` that
defines a ProjectPlugin call envelope is framework-owned protocol. The project
supplies or consumes a document at that boundary, but it does not own the
envelope identity or shape.

This is a fifth ownership category, `operator_protocol`, alongside
`project_supplied`, `wire_tag_owned`, `embedded_fragment` and published schema
files. It follows from the identity namespace and compile site; it does not need
a synthetic wire tag or a `$ref` solely to make ownership machine-readable.

## Context

The generated public contract could classify project-supplied identities,
tag-bearing embedded schemas and referenced fragments directly from source
bytes. Six identities remained: the four ProjectPlugin input envelopes and the
two output envelopes compiled by `project-deployment.cpp`. Their titles and call
sites made their purpose clear, but the four-category taxonomy had no name for
framework-owned protocol without a tag.

Leaving them `unclassified` would make the outward contract less precise than
the implementation. Adding tags would change the wire only to satisfy a
documentation generator. The existing `operator/` identity namespace already
states the stable ownership boundary.

## Consequences

- The public-contract generator classifies those six identities as
  `operator_protocol` from their embedded `$id` values.
- Project-owned payloads nested inside an Operator envelope remain
  `project_supplied`; ownership does not flow from the outer envelope into them.
- A future embedded identity outside the known categories remains visibly
  `unclassified` rather than being guessed.
