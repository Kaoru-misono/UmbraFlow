# Architecture

This file records the shape of the repository: which module owns what, which way
dependencies may point, and what this repository deliberately does not have.

It holds no status and no version. Contract versions, digests and progress
belong to whoever generates them — the consumer's execution ledger, the
manifests, and `schema/`. Dated rulings live in
[`docs/decisions/`](decisions/README.md) and are frozen there rather than
amended here.

## Module direction

The CMake manifest loader turns each direct child of `modules/` carrying a
`manifest.txt` into one library, and `modules/` is the only root that declares
manifests. Those manifests are the dependency-graph authority;
`scripts/check_modules.py` rejects missing edges, cycles and forbidden roots.
This document does not copy the graph.

Three direction rules a human must respect, none of which the manifests state
on their own:

- `core` is the platform-free leaf. Adding or promoting a generic facility to
  `core` requires the repository's core-capability review; Runtime and Operator
  types do not move there merely because two modules use them.
- `schema` depends on nothing, so any module may depend on it. Its framework
  schema catalog is generated from the published files under `schema/`, and it is
  how two modules that cannot link one another still read one document the same
  way. No module owns a second schema spelling.
- `entry/` and `tests/support/` declare no manifest and are outside the graph:
  they compile no library and nothing links them, so they cannot close a cycle.

`modules/conformance` is a library like every other module. It is the logic half
of the second shipped binary, `umbra-flow-conformance`, which is the exported
Operator conformance suite: the binary is `entry/conformance/main.cpp` plus the
module, the same shape as `umbra-flow`. A consuming repository compiles nothing
and reaches no CMake of ours — a project is a directory of data, so a consumer
runs `umbra-flow-conformance --project <directory>` against its own tree, and
`deployment::loadConformanceProject` turns that directory into the five
authorities the suite drives plus the two roles it drives them in. The product's
own verbs take `deployment::loadProductionProject` instead, which reads
`umbraflow-project.json` and the RuntimeArtifact and stops there.
`cmake/conformance-run.cmake` registers one CTest per run inside the
`PROJECT_IS_TOP_LEVEL` guard, because nothing outside this repository reaches it.
Nothing under `modules/conformance/source/` names a project; `examples/umbraflow`
and `examples/arcana-expedition` are two project directories written the way a
consumer writes its own, and this repository is two runs of the suite rather than
its home.

## Ownership

| Area | Sole owner |
|---|---|
| target discovery, capture and native input mechanics | `controller` |
| frame/action ports, observation lease and generation primitives | `engine` |
| VM sandbox and language boundary | `script` |
| trusted RuntimeModel parser, evidence and two-stage resolution | `task/runtime` |
| confined RuntimeArtifact verification and Host generation binding | `task` C++ boundary |
| lease/fence, snapshots, plans, policy | `operator` |
| the Tool Runtime: one admission path, the actor adapters that translate into it, the durable call ledger, dispatch and replay | `operator` |
| production lifecycle composition from a project directory through Operator and Host | `service` |
| ProjectPlugin protocol envelope schemas in the `operator/` identity namespace | `deployment`, as framework-owned protocol |
| published framework schema files and their generated exact-byte runtime catalog | `schema` |
| generic immutable audit events | `trace` |
| offline Project Kit build, immutable release and project command | `project` |
| offline Project Authoring C++ boundary | `authoring` |
| offline evidence, candidates, review, replay and publication | `tools/annotate` |
| the Operator contract a consumer must satisfy, as runnable cases | `conformance` |
| game semantics and payload schemas | external ProjectPlugin consumer |

`project` reaches source bytes only through a caller-owned hash resolver and
verifies every hash itself; neither `project` nor `image` resolves a source
location.

The `project` library is offline. The `project` executable's composition root
alone may acquire the release bundle during `init`; the transport returns files
to the entry, and the library receives only the local project and build paths.
No build, check, freeze or run path reaches the transport.

### Runtime lifetime boundaries

- `service::ProductLifecycle` exclusively owns the production `OperatorTaskHost`,
  control lease, controller binding, runtime generation and plan authority. All
  recoverable setup that can refuse is completed before the lease is acquired. Its
  heap-resident `Impl` is constructed first and stores the acquired lease directly
  as optional active state, so acquisition leaves no fallible ownership transfer.
  `shutdown()` is idempotent and remains the reporting path; `cli::observeProject`
  calls it unconditionally and combines its failure with the work result through
  `reportAfterClose`.
- A live `ProductLifecycle` is move-constructible but not move-assignable: move
  assignment could overwrite an active lease without a return channel for a close
  failure. `ProductLifecycle::Impl` releases any remaining active lease as a
  last-resort RAII fallback. A destructor cannot report that failure, so it does
  not replace the explicit close path. `OperatorTaskHost` serializes lease
  acquire/release/takeover with dispatch, and rolls acquisition back if the Host
  cannot adopt its fence. The next Operator open invalidates process-local lease
  state, but a failed fallback can still omit the expected release transition
  from the audit trail.
- `TaskHost` owns every generation, and there is one kind of them: each carries
  a verified RuntimeArtifact, and the only thing that varies is whether a
  closing record seals that artifact's hash. An unsealed one is a directory
  somebody is still editing, and `installBinding` refuses it, so no
  `RuntimeModelBinding` ever attests to bytes nothing sealed. There is no
  annotation phase and no runtime phase; what a session may do is its
  `tool_closure`'s answer
  (`docs/decisions/2026-08-24-there-is-no-annotation-phase.md`). `cancel()`
  requests
  stop; there is no generation-retirement or quiescence operation. That is
  sufficient while the Host dies with one `ProductLifecycle`, not for a future
  resident Host that reloads generations in place.
- `EngineSession` owns its frame, action and optional OCR providers but borrows
  one stable `TraceRecorder`. `ExplorationSession` makes that relation safe by
  owning the recorder before the context and VM, keeping it behind `unique_ptr`
  and forbidding moves. Other composition roots must preserve the documented
  declaration order until a single aggregate owns this relationship.
- `ExplorationSession::finish()` is the reporting close for `run.finished`; its
  destructor is only structural cleanup. The sole production loop reaches
  `finish()` on one exit path, but the type does not yet enforce that protocol if
  another caller is added.

Host delivery is one trusted call chain:

```text
Operator authority
  -> current task RuntimeModelBinding
  -> exact-cycle opaque Receipt
  -> Host.deliver target critical section
  -> controller native input primitive
```

No other module exports a production input path. The declared input vocabulary
is `click`, `key`, `drag`, `hold`, `scroll` and `move`, and it is the whole of
it: there is no authoring-only half, because anything that changes the target on
delivery is input whoever drives it. What separates an authoring actor from a
production one is the authority it holds, not which verbs exist.

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
  policy-artifact.json (optional; absent is the Operator deny-all)
```

`policy-artifact.json` is the Operator's, and it is the only supply path for a
PolicyArtifact. It sits in the production root rather than in a project
directory or behind a CLI flag because the party a policy protects is the party
that must give it: the machine's owner administers this root, while a project
directory and a command line both belong to whoever is asking to act.

The two SQLite databases are not one transaction domain.

Whether a session may reach the authoring workspace, capture a screen or write
into a project directory is its `tool_closure`'s answer and its operator's
policy grant, not a property of the binary or of the Host object it holds
(`docs/decisions/2026-08-24-there-is-no-annotation-phase.md`). What the
framework owes is that every such grant is loud -- named in the pinned
registration, hashed into the session manifest, and recorded in the ledger --
not that it is impossible. The one thing that is structural is the seal: a
RuntimeModelBinding is only ever built over a hash a closing record covers, so
no run attests to bytes that can still move under it.

## Deliberate absences

Nothing in the code records a decision *not* to build something, so this is the
part of this file that cannot be recovered from anywhere else.

- no Runtime v1, UFR, Page/Element/Hit or Context-truth reader;
- no C++ partial TOML semantic parser;
- no caller-supplied model identity, measurement, effect, risk, Binding,
  coordinate, Receipt, tool mutability or reconciliation disposition: each
  arrives from an authority bound to the exact bytes the ProjectRegistration
  pinned;
- no path that is checked and then opened by name — resource reads and
  deployment staging both resolve once, through held handles that refuse a
  reparse point by attribute;
- no direct run/check/replay production action path;
- no compatibility alias, fallback, dual spelling or dual write;
- no consumer-specific branch in Host, Runtime or Operator;
- no game entity, tool name, state field or content schema in this
  repository's generic core;
- no reading of what a Project's state means. The framework records what
  happened and enforces the limits a registration declared; how a Project's own
  state evolves is the Project's decision, so there is no Journal, no fold, no
  revision and no compare-and-swap over meaning the framework cannot read
  ([the ruling](decisions/2026-08-23-the-framework-stops-interpreting-project-state.md));
- no ambient policy, filesystem, network, package-search or hidden input
  reaching project code. A project's boundary is one closure and nothing else:
  a tool closure exporting the entries its declared Tools bind to, compiled on
  `script::ScopedToolProgram`. The slot is mandatory and a project that binds no
  Tool ships an explicitly empty closure, because an absent slot would be a
  second reading of the document. Closed module resolution and
  registration-pinned read-only resources grant no ambient authority, and the
  closure's one native seam is a synchronous Tool call;
- no gate registered here that requires another repository to be present;
- no developer-authored digest: a hash exists only where an automatically
  produced content identity sits at a real immutable-byte boundary and something
  refuses on mismatch;
- no dynamic plugin ABI, dependency-injection container, message bus,
  distributed lease or generic workflow engine.

The implementation can use fewer files and types than the design prose, but it
cannot merge owners or remove evidence needed by the named contract tests.
