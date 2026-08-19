# Project Kit release manifest

Date: 2026-08-19
Scope: `umbraflow-cpp` plus the consuming repository

The distribution shape is ruled by
[`2026-08-19-project-kit-distribution.md`](../decisions/2026-08-19-project-kit-distribution.md).
This plan owns the remaining work: the release manifest, the publisher step,
the template downloader, and the starting skeleton. It holds work and
acceptance conditions, not consumer status or foreign commit hashes.

## R-01 — Release manifest shape — **settled**

The shape ruling is
[`2026-08-19-release-manifest-shape.md`](../decisions/2026-08-19-release-manifest-shape.md):
the manifest is a wire tag owned by its writer with no schema file, artifact
paths are canonical and `'/'`-only, and the release id is the sha256 of the
manifest's canonical bytes, derived rather than stored. The realized shape is
`scripts/publish_release.py`, whose constants are the authority and whose
member tuples the public contract generator extracts.

## R-02 — Publisher step — **delivered**

`scripts/publish_release.py` enumerates the built binaries for the platform
and arch, computes their sha256, writes the `umbraflow-release/v1` manifest in
canonical JSON with no trailing newline, and prints the derived release id. It
is the only writer of digest values; nothing in a template or a project reads
a digest out of prose. Covered by `tests/test-publish-release.py` (CI
labelled): member shape, deterministic output, the release-id derivation, and
the fail-closed refusals. Publishing beside the artifacts (upload to GitHub
Releases) is the release-day action this script does not automate.

## R-03 — Template downloader — **open, consumer side**

A small Python script in the consumer-owned template that:

- reads the declared contract version from the template,
- fetches the release manifest from GitHub Releases,
- selects the entry matching the host platform/arch and the contract version,
- downloads, verifies sha256, and refuses with the artifact named on mismatch,
- runs `project init/build/check/freeze` with the paths the template declares.

## R-04 — Starting skeleton — **open, consumer side**

The template carries a directory shape and a starting `umbraflow-project.json`
that `project build` accepts. It does not generate the skeleton; `project init`
records declared inputs only.

## Acceptance — status

1. A clean machine fetches the bundle through the downloader, verifies it
   against the manifest, and runs `project init/build/freeze` with no
   hand-edited digest — **not yet reachable**: R-03/R-04 are consumer side.
2. One mutated byte in a downloaded artifact makes the downloader refuse and
   name it — **not yet reachable**: the downloader does not exist yet.
3. A release whose bytes changed requires no template edit; a changed contract
   version is the only template edit that moves — **holds for the publisher**
   (the manifest carries no digest a template would copy); the downloader side
   is R-03.
4. `project` gains no download or process-start capability as part of this
   work — **holds**: no `modules/project` source changed.
