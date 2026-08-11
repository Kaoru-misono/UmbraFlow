# Architecture

> Amended 2026-08-09: the v1.9 spec bundle and
> [breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md)
> replace the former Context/Page/Target runtime description. This file now
> records module ownership only and deliberately does not duplicate wire
> schemas or consumer contracts.

## Authority

Read in this order:

1. [breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md);
2. [migration report](plans/2026-08-09-runtime-migration-report.md);
3. checked-in schemas named by that report.

The read-only consumer bundle is v1.12; its root is
`b3306dde9337a70e5e33bb5676f9da5b0e99b4b1acd2fec1ef4d16dbde51cda5`.
No game entity, tool name, state field, Journal event, or content schema belongs
in this repository's generic core.

> Amended 2026-08-12: the bundle moved three times in three days and this file
> tracked none of them. It read v1.9, root `c4760bb5…bfb6a966`, which was stale
> from v1.10 (2026-08-12, the project-layer amendment), through v1.11 (the
> dual-game waiver, recorded nowhere), to v1.12 (the co-versioned amendment that
> carried that waiver into the upstream design). The `:3` note above said v1.7,
> which was already wrong when written; the correction to v1.9 is
> [cross-repository drift](plans/2026-08-11-cross-repository-drift.md) F-14,
> applied here. `scripts/check_spec_bundle.py` and its authority document still
> pin v1.10 — see [TODO](TODO.md) G0.

## Module direction

April2's manifest loader turns each direct child of `modules/` into one
library. Dependencies remain acyclic:

```text
entry -> operator -> task -> engine -> {controller ports, ocr, vision, trace}
                    \-> script -> {core, domain}
controller -> {core, domain}
vision     -> {core, domain, image}
ocr        -> {core, domain, vision}
trace      -> {core, domain, vision}
image      -> {core, domain}
```

`core` remains the platform-free leaf. Adding or promoting a generic core
facility requires the repository's core-capability review; Runtime/Operator
types do not move there merely because two modules use them.

`conformance/` (added 2026-08-10, renamed from `contract-suite/` on 2026-08-11)
is the one first-party source tree outside `modules/`, `entry/` and `tests/`,
and the loader does not turn it into a library. It builds the second shipped
binary, `umbra-flow-conformance`, which is the exported Operator conformance
suite. A consuming repository compiles nothing and reaches no CMake of ours: a
project is a directory of data, so a consumer runs
`umbra-flow-conformance --project <directory>` against its own tree, and
`deployment::loadProject` turns that directory into the five authorities the
suite drives. `cmake/conformance-run.cmake` registers one CTest per run and is
included inside the `PROJECT_IS_TOP_LEVEL` guard, because nothing outside this
repository reaches it. Nothing under `conformance/source/` or `include/` names a
project; `examples/umbraflow` and `examples/arcana-expedition` are two project
directories written the way a consumer writes its own, and this repository is
two runs of the suite rather than its home.

It carries a `manifest.txt` and is not a module. The autoloader never reads it —
`CPP_MODULE_ROOTS` names `modules/` only — but `scripts/check_modules.py` does,
so the suite is a node in the dependency graph rather than first-party C++
outside it. Before 2026-08-11 a dependency that closed a cycle through the suite
passed while the check printed a module count that was never all the C++.
`tests/support/` is still in that position and declares no manifest.

## Ownership

| Area | Sole owner |
|---|---|
| target discovery, capture and native input mechanics | `controller` |
| frame/action ports, observation lease and generation primitives | `engine` |
| VM sandbox and language boundary | `script` |
| trusted RuntimeModel parser, evidence and two-stage resolution | `task/runtime` |
| confined RuntimeArtifact verification and Host generation binding | `task` C++ boundary |
| lease/fence, snapshots, plans, policy, approvals, Operation and reconciliation | `operator` |
| generic immutable audit events | `trace` |
| offline evidence, candidates, review, replay and publication | `tools/annotate` |
| the Operator contract a consumer must satisfy, as runnable cases | `conformance` |
| game semantics and payload schemas | external ProjectPlugin consumer |

Host delivery is one trusted call chain:

```text
Operator authority
  -> current task RuntimeModelBinding
  -> exact-cycle opaque Receipt
  -> Host.deliver target critical section
  -> controller native input primitive
```

No other module exports a production click/key/drag path.

## Artifact and database boundary

```text
offline authoring
  annotation-workspace.sqlite + frames/replay/private blobs
      -> immutable RuntimeArtifact release
      -> deployment principal verifies/copies
      -> production installed generation

production
  RuntimeArtifact (runtime-model.toml + manifest-listed assets)
  operator-runtime.sqlite
```

Production cannot attach, traverse or read the authoring workspace. Runtime
artifacts, sessions and traces never carry annotation screenshots. The two
SQLite databases are not one transaction domain.

## Deliberate absences

- no Runtime v1, UFR, Page/Element/Hit or Context-truth reader;
- no C++ partial TOML semantic parser;
- no caller-supplied model identity, measurement, effect, risk, Binding,
  coordinate, Receipt, tool mutability, reducer input or reconciliation
  disposition: each arrives from an authority bound to the exact bytes the
  ProjectRegistration pinned;
- no path that is checked and then opened by name — artifact reads and
  deployment staging both resolve once, through held handles that refuse a
  reparse point by attribute;
- no direct run/check/replay production action path;
- no compatibility alias, fallback, dual spelling or dual write;
- no consumer-specific branch in Host, Runtime or Operator;
- no dynamic plugin ABI, dependency-injection container, message bus,
  distributed lease or generic workflow engine.

The implementation can use fewer files and types than the design prose, but it
cannot merge owners or remove evidence needed by the named contract tests.
