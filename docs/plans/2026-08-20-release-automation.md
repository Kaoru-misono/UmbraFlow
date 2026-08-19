# Release automation

Date: 2026-08-20
Scope: `umbraflow-cpp`

This plan owns the release-day step the Project Kit distribution plan left
unautomated: uploading a built release bundle to GitHub Releases. Lifted from
the archived
[`2026-08-19-project-kit-release-manifest.md`](../archive/plans/2026-08-19-project-kit-release-manifest.md),
which delivered the publisher, the manifest and the downloader but not the
upload.

## R-01 — Automated tag-driven release

A `git tag v*` push must run the full local gate, build the `x64-release`
preset, generate the manifest with the tag as the release name, and publish
the release and its assets. The version has one source: the tag, which must
equal the manifest's `release` member and the GitHub release tag, because the
template downloader builds asset URLs from that member.

Design notes for the implementation:

- the upload should read the manifest's `asset` members, which are already the
  flat names a GitHub release serves;
- `publish_release.py` may grow a mode that prepares the upload directory so
  the workflow uploads a flat directory rather than re-deriving names;
- the release job needs MSVC, the same toolchain the local gate uses;
- a draft release published for manual confirmation is a reasonable
  middle-ground, but tag-is-release is acceptable for this scale.

## Acceptance

1. Pushing a `v*` tag creates a GitHub release whose tag, manifest `release`
   member and asset names all agree.
2. A failing gate aborts the release.
3. A clean machine still fetches that release through the template downloader.
