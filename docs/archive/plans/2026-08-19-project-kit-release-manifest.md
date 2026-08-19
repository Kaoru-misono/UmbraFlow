# Project Kit release manifest

Date: 2026-08-19
Scope: `umbraflow-cpp` plus the consuming repository
Status: **archived 2026-08-20** — all work delivered and accepted; the one
surviving obligation (release automation) is lifted to
[`2026-08-20-release-automation.md`](2026-08-20-release-automation.md).

The distribution shape is ruled by
[`2026-08-19-project-kit-distribution.md`](../decisions/2026-08-19-project-kit-distribution.md).
This plan owned the release manifest, the publisher step, the template
downloader, and the starting skeleton.

## R-01 — Release manifest shape — **settled**

The shape ruling is
[`2026-08-19-release-manifest-shape.md`](../decisions/2026-08-19-release-manifest-shape.md):
the manifest is a wire tag owned by its writer with no schema file, artifact
paths are canonical and `'/'`-only, and the release id is the sha256 of the
manifest's canonical bytes, derived rather than stored. The realized shape is
`scripts/publish_release.py`, whose constants are the authority and whose
member tuples the public contract generator extracts.

## R-02 — Publisher step — **delivered**

`scripts/publish_release.py` enumerates the built binaries and the runtime
payload (`onnxruntime*.dll`, `models/**/*`) for the platform and arch,
computes their sha256, writes the `umbraflow-release/v1` manifest in canonical
JSON with no trailing newline, and prints the derived release id. An artifact
row carries both the restore `path` and the flat `asset` name a GitHub
release serves it under. It is the only writer of digest values. Covered by
`tests/test-publish-release.py` (CI labelled): member shape, deterministic
output, the release-id derivation, and the fail-closed refusals. Uploading
beside the artifacts is the release-day action this script does not automate.

## R-03 — Template downloader — **delivered, consumer side**

`fetch-umbraflow.py` in the consumer-owned template reads the declared
contract version from `umbraflow-project.json`, fetches the latest release
manifest from the host, refuses a release that does not carry that contract
version, selects the artifacts for the host platform and arch, verifies each
sha256 and refuses with the artifact named on mismatch, restores every file at
its manifest path (payload beside the binaries), and drives
`project init/build/check/freeze`. Covered by
`tests/test_fetch_umbraflow.py` over `file://` releases.

## R-04 — Starting skeleton — **delivered, consumer side**

The template ships the neutral skeleton (`umbraflow-project.json`, schemas
under `schemas/<plugin-id>/`, a content blob) and no plugin of either form;
`scaffold --plugin generated|hand-written` is the initialization step that
writes the chosen plugin form and configures the primary deployment. The
declared inputs are derived from the deployment declaration, never copied.

## Acceptance — all met

1. A clean machine fetches the bundle through the downloader, verifies it
   against the manifest, and runs `project init/build/check/freeze` with no
   hand-edited digest — **met** by a real end-to-end run against the v0.1.1
   GitHub release.
2. One mutated byte in a downloaded artifact makes the downloader refuse and
   name it — **met** by the downloader's own test.
3. A release whose bytes changed requires no template edit; a changed contract
   version is the only template edit that moves — **met**: the manifest
   carries no digest a template copies, and the real run proved the contract
   check.
4. `project` gains no download or process-start capability as part of this
   work — **met**: no `modules/project` source changed.
