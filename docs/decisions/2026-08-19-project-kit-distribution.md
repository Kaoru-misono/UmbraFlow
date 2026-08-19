# 2026-08-19 — The Project Kit is distributed as a release bundle consumed by a consumer-owned template

## Decision

The offline Project Kit (`project`) and the runtime it feeds are distributed as
one versioned release bundle per platform and architecture. A publisher step in
this repository writes an immutable `umbraflow-release/v1` manifest naming each
artifact and its content sha256; no developer authors a digest, and the
template's downloader parses the manifest, selects the matching entry, and
refuses a mismatch.

A project template is a consumer-owned repository, forked per game. It declares
the project contract version it targets and resolves "the latest release
compatible with it" from the release manifest; it never pins an exe revision or
digest. A generic framework-owned template waits for a second real consumer.

The downloader is a small Python script run before any `project` command. The
`project` executable does not download, does not start processes, and does not
resolve a download location; those stay in the template script.

Releases are hosted on GitHub Releases and named with milestone names, not
semver, while no packaged release exists.

## Context

The `project` executable already exists as a separate binary
(`entry/CMakeLists.txt`), not linked into `umbra-flow`, with the verbs
`init/build/check/freeze/run`. What is missing is the distribution layer: there
is no versioned release, no download location, and no verified way for a fresh
checkout to obtain the binaries.

The shape was evaluated against three frozen rulings:

- process orchestration is not an artifact family, and no `modules/project`
  source starts a process
  ([`2026-08-17-orchestration-is-not-an-artifact-family.md`](2026-08-17-orchestration-is-not-an-artifact-family.md));
- developers author no hash values, and compatibility selection uses format
  versions rather than exact digests
  ([hash admission rule](../2026-08-14-hash-management-simplification-proposal.md));
- consumer examples never become core contracts
  ([`2026-08-09-consumer-examples-never-become-core-contracts.md`](2026-08-09-consumer-examples-never-become-core-contracts.md)).

A template that pinned an exact exe digest would recreate the removed
cross-repository spec-bundle pin. A template that generated the project skeleton
would contradict `init`, which records declared inputs and never creates
`umbraflow-project.json`. A downloader that let the executable spawn would break
the orchestration ruling.

## Consequences

- Distribution is a release bundle, not a single exe: a project author needs
  `project` to build/freeze and `umbra-flow` to run.
- The release manifest is the single authority a downloader trusts; its sha256
  over downloaded bytes is the first-stage integrity check, with cryptographic
  signatures as later hardening.
- The template owns orchestration and the starting skeleton (directory shape
  plus a starting `umbraflow-project.json`); `project` owns validation and
  generation.
- The framework ships the release bundle plus the sample skeletons under
  `examples/`; it ships no game-specific template.
