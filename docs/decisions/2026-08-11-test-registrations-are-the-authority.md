# 2026-08-11 — The CTest registrations are the authority; no document carries a gate count

## Decision

`UF_REQUIRED_DOCTEST_CONTRACTS` in `tests/CMakeLists.txt` and the `CASES` lists
beside it are the authority for which gates exist. No document states a gate
count, a requirement count, or a closure count. A requirement-to-gate map is a
map, not a closure ledger, and must not be read as one.

Two gate spellings exist and mean different things: `contract-<area>-<id>`
exercises the code and goes red when the behaviour is removed;
`schema-<area>-<id>` reads a `schema/*.json` file and asserts a definition exists
with certain members, so it passes whether or not the behaviour does. A
requirement may own one of each, because the behaviour and the schema shape are
different things to guard.

## Context

This was ruled after the same drift recurred six times with no counter-example.
Every commit that had ever registered a gate updated `tests/CMakeLists.txt` and
left the migration report's rows and totals behind — `dcc43b5`, found by the
third adversarial round as R3-F3; then `4b955de` and `848e390`; then `25f57f9`,
`93698b4` and `c23efd3`. Six commits, one direction.

The cause is structural rather than individual. `tests/CMakeLists.txt` is
machine-checked and refuses to disagree with itself. **No gate reads the
report**, so the ordering "report first, then the registrations" is enforced by
nothing, and the report is always the side that drifts.

The transcribed counts made it worse than merely stale. Reading a per-requirement
`contract-` gate as proof of closure produced two wrong conclusions in one day, in
opposite directions: `A-07` was declared reopened on a misreading that
substituted a requirement sentence for an acceptance clause (`07abc3e`), then
restored when the acceptance text was read correctly (`bed456f`). A separate
"40 of 42" figure invited the conclusion that two requirements were
unimplemented, when what those two lacked was a per-requirement CTest ID for the
behavioural half — the behaviour and its aggregate gate both existed, and
`UF_SCHEMA_ONLY_REQUIREMENTS` names exactly those two so CMake refuses to let the
set drift.

## Consequences

- A document may name gate IDs, because a named ID is checkable by `ctest -N`. It
  may not state how many there are.
- If gate-map drift recurs, the answer is a check that reads the map, not a
  stricter reading of a stop condition. A rule enforced by nothing is not a rule.
- This is the narrow case of the general rule ruled later:
  [2026-08-19 — a document holds no fact that something else verifies](2026-08-19-documents-hold-no-foreign-facts.md).
