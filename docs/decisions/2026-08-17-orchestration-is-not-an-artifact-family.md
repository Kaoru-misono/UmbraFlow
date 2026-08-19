# 2026-08-17 — Orchestration is a different axis from artifact families

## Decision

Process orchestration is not an artifact family, and giving the offline Project
Kit the ability to start processes is a **new architectural means, not the filling
of a gap**. It requires a ruling, not an implementation.

## Context

Prose had drifted into treating "tool orchestration" as one of the Kit's
artifact families, alongside the generated adapter and the tool catalog. It is
not the same kind of thing: a family is a set of generated artifacts inside the
Kit's digest closure, and orchestration is running another program.

Measured: `modules/project/` contains no `system(`, `CreateProcess`, `popen`,
`subprocess` or `spawn`, and `modules/project/manifest.txt` declares no dependency
that would supply one. No framework module starts a process.

The orchestration that does exist is outside the Kit and already owned elsewhere:
the offline annotation tooling invokes the `project` executable across its verbs
and names the failing candidate, which is annotation-side work. Content-side
orchestration is a consumer-run compiler that `modules/project/` does not
reference at all.

Two related transcription errors were corrected in the same review and are
recorded here because the corrections are the useful part:

**A quantity with no source.** A claimed count of artifact families existed in no
file in either repository — `git log --all -S` over the full history matched zero
commits, and it survived only in commit messages. Every candidate list was a
different number, and the one enumerable and enforced list was a different thing
again, and had itself changed size. A number nobody can trace must not enter a
schedule.

**A commit message that contradicted itself, explicably.** Two documents share one
name: the *framework* schema catalog is the generated index of `schema/`, and the
*project* schema catalog is the project's own declared documents. A title naming
one and a body calling the other ungenerated are both true.

## Consequences

- Progress on the Kit is stated per named artifact, never as a family count. Of
  the three often cited, only the adapter and the tool catalog are
  project-declared; the framework schema catalog is a constant, byte-identical
  for every project.
- A mechanism reachable from the library but not from the executable is not
  delivered. The tool catalog was wired into the library while the CLI passed an
  empty catalog set, so the executable produced an empty directory and only test
  support filled it.
- The Kit's negative cases are genuinely sound: eight mutations on a copy each
  exit non-zero and name the single offending artifact — appending a byte to the
  adapter, changing a key in the framework schema catalog, altering the
  artifact-roots registration, deleting the adapter, adding a file to a family,
  planting a hand-written file among generated tool catalogs, and editing either
  the artifact manifest or a declared input.
