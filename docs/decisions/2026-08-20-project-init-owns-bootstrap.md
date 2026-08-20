# 2026-08-20 — `project init` owns release bootstrap and the starter project

This decision supersedes
[`2026-08-19-project-kit-distribution.md`](2026-08-19-project-kit-distribution.md)
and the “no C++ reader” part of
[`2026-08-19-release-manifest-shape.md`](2026-08-19-release-manifest-shape.md).
It supplies the process-start ruling required by
[`2026-08-17-orchestration-is-not-an-artifact-family.md`](2026-08-17-orchestration-is-not-an-artifact-family.md)
for release acquisition only.

## Decision

A project author obtains one `project` executable before entering the project
workflow. From that point, `project init` is the single bootstrap command. It
reads the project-owned release location, installs and verifies the matching
release bundle when one is not already locked locally, generates the starter
project when the root document is absent, derives the initialization inputs,
and writes the Project Kit input ledger.

The template owns no downloader, release-manifest reader, project schema
snapshot, plugin scaffold or derived-input algorithm. It is a thin repository
that carries only the stable acquisition location, ignore rules and human
entry instructions. A real project keeps its authored root document, modules,
resources and schemas; rerunning `init` verifies and records those bytes rather
than replacing them.

Release transport belongs to the `project` executable's composition root. It
may start the host `curl` program with an argument vector to retrieve an HTTPS
release or an explicit `file://` mirror, but `modules/project` remains an
offline library and no build, check, freeze or run operation can reach that
transport. Redirects remain HTTPS-only. The release-manifest parser, path
confinement, digest verification and scaffold generation are C++ code shipped
and tested with the executable.

An installed bundle is immutable. Its exact release-manifest bytes live beside
the artifacts they name; a later `init` verifies that local closure and does
not silently follow a newer release. A missing bundle may be installed, but a
present incomplete or mismatched bundle is refused by artifact name rather
than repaired in place. Refreshing means acquiring a new bootstrap executable
and installing into a clean bundle root.

## Context

The consumer template had become a second implementation of the Project Kit
contract. It parsed the release manifest, selected platform artifacts, verified
digests, generated both plugin forms, derived inputs and restated command
defaults. Every Project Kit contract change therefore required coordinated
framework, template and consumer edits, and nothing connected those edits into
one failing gate.

Automating copies or pull requests would move that coordination into a bot
without removing either implementation. The executable already owns project
validation and generated artifacts, and its `init` path already derives module
and resource inputs and writes starter Luau modules. Giving that same shipped
version the remaining bootstrap decisions removes the duplicated authority.

The executable cannot download itself before it exists. That first acquisition
is the irreducible bootstrap boundary and is deliberately outside the project
format: a package manager, release download, source build or another trusted
installation may provide it. Once it is running, no project-owned Python
program is needed.

## Consequences

- `project init` is safe to repeat: it writes a starter only into an
  uninitialized root, never overwrites an authored project, and verifies a
  locked local release before recording inputs.
- The release manifest remains the publisher's immutable wire document, but it
  now has a shipped C++ consumer in addition to its Python publisher.
- `project init` derives normal inputs from the project declaration. Explicit
  extra inputs remain available for authoring generators whose source bytes do
  not belong in the runtime declaration.
- The starter shape changes in the same release as the Project Kit contract
  that generates and validates it. A template repository update is no longer
  part of that atomic cut.
- HTTPS/file release reads and process execution are acquisition capabilities
  of one executable entry, not ambient capabilities of ProjectPlugin, Runtime,
  Operator or the offline Project Kit library.
