# 2026-08-12 — The conformance suite is a module, and nothing declares a manifest outside `modules/`

## Decision

The exported Operator conformance suite is a library under `modules/` like every
other module, and `modules/` is the single root that carries manifests. Nothing
outside it declares one.

The project loader is split in the same ruling:
`deployment::loadConformanceProject` turns a project directory into the
authorities the suite drives and the roles it drives them in, while the
product's own verbs take `deployment::loadProductionProject`, which reads
`umbraflow-project.json` and the RuntimeArtifact and stops there.

## Context

Until this date the suite was `conformance/` at the repository root — the one
first-party source tree outside `modules/`, `entry/` and `tests/`. It carried a
`manifest.txt` the CMake autoloader never read, and `scripts/check_modules.py`
reached it through a `DECLARED_SOURCE_TREES` list. A manifest nothing reads is a
declaration with no verifier behind it, and a second declared root meant the
dependency-graph check had two entry points and could only be as strong as the
weaker one. Three C++ fixtures inside it belonged to `tests/support/`.

The loader split had its own defect to remove: before it, one loader demanded a
conformance fixture of every directory the product opened. Production and
conformance ask different things of a project directory, and a single loader made
the product pay the suite's admission price.

The suite was added on 2026-08-10 as `contract-suite/`, renamed `conformance/` on
2026-08-11, and moved under `modules/` on this date.

## Consequences

- `scripts/check_modules.py` is back to one root, and the `DECLARED_SOURCE_TREES`
  list is gone.
- `tests/support/` declares no manifest and stays outside the dependency graph;
  it compiles no library and nothing links it.
- `modules/conformance` is the logic half of the second shipped binary,
  `umbra-flow-conformance` — `entry/conformance/main.cpp` plus the module, the
  same shape as `umbra-flow`.
- A consuming repository compiles nothing and reaches no CMake of ours: a project
  is a directory of data, so a consumer runs
  `umbra-flow-conformance --project <directory>` against its own tree.
  `cmake/conformance-run.cmake` registers one CTest per run inside the
  `PROJECT_IS_TOP_LEVEL` guard, because nothing outside this repository reaches
  it.
