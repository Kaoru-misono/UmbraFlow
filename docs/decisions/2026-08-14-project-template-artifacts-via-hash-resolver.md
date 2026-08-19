# 2026-08-14 — Project generates template artifacts through a caller-owned hash resolver

## Decision

`project` depends on `image` to generate template artifacts from declarations
that carry a template path, source content hashes and a crop rectangle.
`project` receives source bytes through a caller-owned hash resolver, verifies
each hash, and hands decoded images to `image`. Neither module resolves source
locations.

## Context

Template generation needs source pixels, and the obvious shape — let the module
that needs the bytes go find them — puts filesystem knowledge inside two modules
that otherwise have none, and makes the outcome of a generation depend on ambient
state that leaves no record of what produced it. Content hashes are already the
project's identity for source material, so the only thing the module genuinely
needs is a way to turn a hash into bytes; who holds those bytes and where is the
caller's problem.

Verification stays inside `project` rather than being trusted to the resolver: a
resolver is caller-supplied, and a boundary that accepts caller-supplied bytes
without checking them against the hash the declaration pinned is not a boundary.

## Consequences

- `project -> image` is a real edge in the dependency graph.
- No path here is checked and then opened by name, and neither module reads a
  location out of a declaration.
- A caller that cannot produce the bytes for a pinned hash gets a refusal, not a
  fallback lookup.
