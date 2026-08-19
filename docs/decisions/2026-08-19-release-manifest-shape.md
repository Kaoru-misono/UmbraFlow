# 2026-08-19 — The release manifest shape: wire tag, path discipline, content identity

## Decision

The Project Kit release manifest is an immutable document tagged
`umbraflow-release/v1`, written only by `scripts/publish_release.py` and never
authored by hand. It has **no schema file in the runtime catalog**: no C++
reader consumes it, and embedding it would change every generated
`framework-schema-catalog-v1.json` for a document no runtime reads. It is a
wire tag owned by its writer, exactly as the project kit's own manifests
(`umbraflow-project-kit-artifact-manifest/v1`,
`umbraflow-project-kit-artifact-registration/v1`) already are. The shape
authority is the publisher's constants, and the member set is extracted from
them into the public contract so a consumer's downloader has one authored
spelling to read.

An artifact `path` is canonical, `'/'`-only, and relative to the release root,
on the same terms the project manifest's path discipline demands. The
publisher writes it; the downloader refuses anything else.

The release id is the manifest's own content identity: the sha256 of the
manifest's canonical bytes, derived by the publisher and printed, never stored
inside the manifest. A digest inside its own document would be a
self-referential copy of bytes already present, the same reason the project
release derives its id from its artifact manifest. The manifest is written in
RFC 8785 JCS form with no trailing newline, so the id is a function of the
exact bytes on disk.

## Context

The distribution decision
([`2026-08-19-project-kit-distribution.md`](2026-08-19-project-kit-distribution.md))
ruled the bundle and the template but left two design points open: whether the
manifest's `path` follows the project manifest's path discipline, and whether
the release id is the manifest's own content identity or a separate field.
Both are settled here, plus the third question that appeared while
implementing: whether the manifest gets a schema file in the runtime catalog.

The catalog test (`tests/deployment/test-project-deployment.cpp`) pins the
catalog to its current size and every catalog entry lands in every project's
generated `framework-schema-catalog-v1.json`. Adding a document no C++ reader
uses would pay that cost for nothing. The kit's own manifests already set the
precedent for wire tags without schema files.

## Consequences

- The publisher script is the only writer and the shape authority; the
  template's downloader parses its members and verifies sha256 defensively,
  refusing a wrong shape rather than reading garbage.
- The release id is reproducible: anyone re-running the publisher on the same
  binaries derives the same id, because it is a function of the manifest bytes
  alone.
- A new manifest member must be added to the publisher constants and gain a
  meaning in `scripts/generate_public_contract.py`, or the public contract
  generation fails.
