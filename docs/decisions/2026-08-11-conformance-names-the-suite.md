# 2026-08-11 — `conformance` names the suite; specification forks are "executable specification resolutions"

## Decision

The word `conformance` in this repository names exactly one thing: the exported
suite. The four frozen choices that resolve contradictions inside the consumer's
specification are called **executable specification resolutions**, renamed from
"executable conformance resolutions".

## Context

A word given both to a test suite and to a class of specification fork is the
defect this rename exists to remove. The suite took the word for two reasons: the
wider audience wins it — every future consumer reads the suite's name, while the
resolution term is read by this repository's maintainers — and "resolution of a
contradiction inside a frozen specification" is what the four actually are, which
the new name says and the old one did not.

Nothing about the content of the four resolutions changed.

## Consequences

- The term is recorded in `CONTEXT.md`, with "executable conformance resolution"
  listed under `_Avoid_`.
- The suite's own module and binary took the name in the same period: added
  2026-08-10 as `contract-suite/`, renamed `conformance/` on this date, moved
  under `modules/` on 2026-08-12 — see
  [2026-08-12](2026-08-12-conformance-suite-is-a-module.md).
