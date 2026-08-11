# Architecture

> Amended 2026-08-09: the v1.7 spec bundle and
> [breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md)
> replace the former Context/Page/Target runtime description. This file now
> records module ownership only and deliberately does not duplicate wire
> schemas or consumer contracts.

## Authority

Read in this order:

1. [breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md);
2. [migration report](plans/2026-08-09-runtime-migration-report.md);
3. checked-in schemas named by that report.

The read-only consumer bundle is v1.9; its root is
`c4760bb59e7df28e13a676446a4cfbb4a62b067741420ecf13f4b939bfb6a966`.
No game entity, tool name, state field, Journal event, or content schema belongs
in this repository's generic core.

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
and the loader does not turn it into a library. It is the exported Operator
conformance suite: a consumer repository includes `cmake/conformance-suite.cmake`,
writes one translation unit defining
`uf::operator_runtime::conformance::provideProject` against the single public
header `conformance/include/conformance/provider.hpp`, and calls
`uf_add_conformance_suite()`. The suite's own sources compile into the
consumer's executable rather than shipping as a library, because a conformance
run means nothing unless it carries the consumer's safety profile and
sanitizers. Nothing under `conformance/source/` or `include/` names a project;
the two exemplars in `conformance/exemplars/` are ordinary consumers of the same
entry point, and this repository is one run of the suite rather than its home.

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
  RuntimeArtifact (page-model.toml + manifest-listed assets)
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
