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

The read-only consumer bundle root is
`90496173f8a920e59659b7c4568c8d04abe7b4d3d209db493a59fc3c55226ed0`.
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
  coordinate, Receipt, tool mutability or reducer input — but the
  reconciliation disposition is still one, which
  [the 2026-08-10 review](reviews/2026-08-10-runtime-hardening-review.md)
  records as open;
- no direct run/check/replay production action path;
- no compatibility alias, fallback, dual spelling or dual write;
- no consumer-specific branch in Host, Runtime or Operator;
- no dynamic plugin ABI, dependency-injection container, message bus,
  distributed lease or generic workflow engine.

The implementation can use fewer files and types than the design prose, but it
cannot merge owners or remove evidence needed by the named contract tests.
