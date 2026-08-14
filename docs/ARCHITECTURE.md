# Architecture

> Amended 2026-08-13: the Runtime v2 spec bundle and
> [breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md)
> replace the former Context/Page/Target runtime description. This file now
> records module ownership only and deliberately does not duplicate wire
> schemas or consumer contracts.

## Authority

Read in this order:

1. [breaking rewrite authority](plans/2026-08-09-runtime-hardening-rewrite.md);
2. [migration report](plans/2026-08-09-runtime-migration-report.md);
3. checked-in schemas named by that report.

The read-only consumer bundle is v1.18; its root is
`ac8c3fa652fb1601645d0c0bc04359bc75c9d08dc2883aa31ddeb94912f38ec4`.
No game entity, tool name, state field, Journal event, or content schema belongs
in this repository's generic core.

> Amended 2026-08-12: the bundle moved three times in three days and this file
> tracked none of them. It read v1.9, root `c4760bb5…bfb6a966`, which was stale
> from v1.10 (2026-08-12, the project-layer amendment), through v1.11 (the
> dual-game waiver, recorded nowhere), to v1.12 (the co-versioned amendment that
> carried that waiver into the upstream design). The `:3` note above said v1.7,
> which was already wrong when written; the correction to v1.9 is
> [cross-repository drift](archive/plans/2026-08-11-cross-repository-drift.md) F-14,
> applied here.
>
> *(Corrected 2026-08-12, later the same day: this note ended "`scripts/check_spec_bundle.py`
> and its authority document still pin v1.10 — see TODO G0". The full
> `python scripts/check_spec_bundle.py` gate now reads the real consumer bundle
> against the root pin. That TODO row is ticked.)*
>
> *(Corrected again 2026-08-12: v1.13 records the consumer repository's
> product/conformance split and archived evidence paths. The hardening rewrite
> and `scripts/check_spec_bundle.py` moved together to root `c8e559a1…ec6e5f0`.)*
>
> *(Corrected 2026-08-13: the current pin is v1.18 at the root stated above.
> The earlier values in this dated amendment are historical evidence, not
> alternate accepted pins.)*

## Module direction

April2's manifest loader turns each direct child of `modules/` into one
library. Dependencies remain acyclic:

> Amended 2026-08-13: the
> [framework schema catalog](../modules/schema/source/schema/framework-schema-catalog.hpp)
> is a leaf module generated from the published files under `schema/`.
> Deployment, Operator and Project depend on that one runtime catalog; none
> owns a second schema spelling.
>
> Amended 2026-08-14: Project depends on Image to generate template artifacts
> from declarations containing a template path, source content hashes and a
> crop rectangle. Project receives source bytes through a caller-owned hash
> resolver, verifies each hash, and gives decoded images to Image; neither
> module resolves source locations.
>
> Amended 2026-08-14 at `dc109bd`: the graph below now includes the `service`,
> `authoring`, `project`, `cli` and `conformance` modules that the earlier
> summary omitted. This is a factual correction to the manifest graph, not an
> approval of the separate HostPlugin architecture proposal.

```text
entry/cli         -> {cli, core}
entry/project     -> {project, core}
entry/conformance -> conformance

authoring   -> {core, domain, image, json, task}
cli         -> {engine, task, deployment, image, json, operator, service, trace}
                \-> controller on Windows
conformance -> {core, deployment, domain, engine, image, operator, script,
                task, trace}
service     -> {core, deployment, operator, task, domain, engine, ocr, schema,
                trace}
deployment  -> {core, domain, json, operator, task, image, schema}
project     -> {core, domain, operator, image, json, schema}
operator    -> {core, domain, json, task, trace, script, schema}
task        -> {core, domain, engine, ocr, script, trace, image}
engine      -> {core, domain, ocr, trace, vision}
script      -> {core, domain, json}
ocr         -> {core, domain, vision}
vision      -> {core, domain, image}
controller  -> {core, domain}
trace       -> {core, domain}
image       -> {core, domain}
json        -> core
domain      -> core
schema      -> {}
```

`core` remains the platform-free leaf. Adding or promoting a generic core
facility requires the repository's core-capability review; Runtime/Operator
types do not move there merely because two modules use them.

`modules/conformance` (added 2026-08-10 as `contract-suite/`, renamed
`conformance/` on 2026-08-11, and moved under `modules/` on 2026-08-12) is a
library like every other module. It is the logic half of the second shipped
binary, `umbra-flow-conformance`, which is the exported Operator conformance
suite; the binary is `entry/conformance/main.cpp` plus `modules/conformance`,
the same shape as `umbra-flow`. A consuming repository compiles nothing and
reaches no CMake of ours: a
project is a directory of data, so a consumer runs
`umbra-flow-conformance --project <directory>` against its own tree, and
`deployment::loadConformanceProject` turns that directory into the five
authorities the suite drives plus the two roles it drives them in. The product's
own verbs take `deployment::loadProductionProject` instead, which reads
`umbraflow-project.json` and the RuntimeArtifact and stops there (split
2026-08-12; before it one loader demanded a conformance fixture of every
directory the product opened). `cmake/conformance-run.cmake` registers one CTest per run and is
included inside the `PROJECT_IS_TOP_LEVEL` guard, because nothing outside this
repository reaches it. Nothing under `modules/conformance/source/` names a
project; `examples/umbraflow` and `examples/arcana-expedition` are two project
directories written the way a consumer writes its own, and this repository is
two runs of the suite rather than its home.

> Amended 2026-08-12: until that date the suite was `conformance/` at the
> repository root, the one first-party source tree outside `modules/`, `entry/`
> and `tests/`, carrying a `manifest.txt` the autoloader never read and
> `scripts/check_modules.py` reached through a `DECLARED_SOURCE_TREES` list.
> Both are gone: nothing carries a manifest outside `modules/` now, the check
> is back to a single root, and the three C++ fixtures moved to `tests/support/`.
> See [TODO](TODO.md), "Build-system shape". `tests/support/` declares no
> manifest and is still outside the graph.

## Ownership

| Area | Sole owner |
|---|---|
| target discovery, capture and native input mechanics | `controller` |
| frame/action ports, observation lease and generation primitives | `engine` |
| VM sandbox and language boundary | `script` |
| trusted RuntimeModel parser, evidence and two-stage resolution | `task/runtime` |
| confined RuntimeArtifact verification and Host generation binding | `task` C++ boundary |
| lease/fence, snapshots, plans, policy, approvals, Operation and reconciliation | `operator` |
| production lifecycle composition from a project directory through Operator and Host | `service` |
| published framework schema files and their generated exact-byte runtime catalog | `schema` |
| generic immutable audit events | `trace` |
| offline Project Kit build, immutable release and project command | `project` |
| offline Project Authoring C++ boundary | `authoring` |
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
