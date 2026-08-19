# 2026-08-14 — The recorded dependency graph is read from the manifests

## Decision

The dependency graph written in `docs/ARCHITECTURE.md` is the graph the module
manifests declare, read from them rather than summarized from memory. The
correction made at `dc109bd` added the `service`, `authoring`, `project`, `cli`
and `conformance` modules that the earlier hand-written summary had omitted.

## Context

A hand-written summary of a machine-checked graph is a transcription, and this
one had silently fallen five modules behind while continuing to read as
complete — a reader could not tell that the omissions were omissions. The graph
itself is verified by `scripts/check_modules.py` against the manifests, so the
document's only defensible job is to state the *direction rules* a human must
respect, with the roster read off the manifests when it is written down at all.

This was recorded as a factual correction to the graph. It was explicitly **not**
an approval of the separate HostPlugin architecture proposal, which the presence
of a `service` module in the graph might otherwise be read as endorsing; that
proposal was ruled on separately and archived.

## Consequences

- `core` is the platform-free leaf and `schema` depends on nothing; both are
  enforced by `scripts/check_modules.py`, not by prose.
- Adding or promoting a generic `core` facility requires the repository's
  core-capability review. Runtime or Operator types do not move to `core` merely
  because two modules use them.
- The graph in the design document is a convenience for readers; the manifests
  under `modules/*/manifest.txt` are the authority, and the check is what reds.
