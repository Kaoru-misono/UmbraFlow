# HostPlugin architecture proposal — 2026-08-14

> **Status: revised proposal, not current execution authority.** This document
> records a candidate architecture against repository commit `dc109bd`. It
> neither changes the current contract in [ARCHITECTURE.md](ARCHITECTURE.md)
> nor opens a second unfinished-work list. If an option is approved, its work
> must first enter the consumer repository's canonical execution plan.
>
> Revised 2026-08-14 after independent review and developer direction to avoid
> over-engineering and remove hand-maintained hash management. The revision
> adds alternatives, prices the low-cost correctness repair separately,
> preserves the CLI safety gate, defines the error and generation models, and
> removes the proposed runtime service graph and composition hash.

## Decision requested

Choose among three independently priced outcomes:

| Option | Outcome | Estimate |
|---|---|---:|
| A. Correctness only | close ProductLifecycle on every exit and put the real SessionManifest identity in trace | 2–4 person-days |
| B. Narrow HostPlugin pilot | A plus a product-local static typed-install shell and one real observe vertical slice | 10–17 person-days total |
| C. Repository HostPlugin migration | B plus typed profiles, remaining runtime behavior, command safety-gate migration and hardening | 30–52 person-days total |

Recommendation:

1. do A regardless of the plugin decision;
2. approve at most B initially;
3. approve C only if B demonstrates less duplicated composition and clearer
   ownership without weakening authority or developer experience.

Option A repairs known defects. It does not provide HostPlugin composition.
Option B purchases evidence for the architecture. Option C is not implied by
approving B.

## Proposed organizing principle

For UmbraFlow's own product runtime:

> Every composable product-runtime behavior or long-lived extension point is
> installed through a statically linked **HostPlugin** unit. Algorithms,
> values, protocols and per-operation objects remain ordinary C++ library
> code.

This is inspired by DeepSeek Harness and Cordis organization, not a Cordis
port. The pilot deliberately has:

- no DLL or out-of-tree C++ loading;
- no universal service locator or dependency-injection container;
- no runtime service dependency solver;
- no automatic unload/reload when a dependency disappears;
- no HMR;
- no composition hash;
- no requirement that every CMake module or class become a HostPlugin.

The facility remains product-local beside `service::ProductLifecycle`. It is
not promoted into `core`.

## Two plugin systems, never one

UmbraFlow already has a product-project extension boundary named
`ProjectPlugin`. HostPlugin is a separate system in vocabulary, trust,
composition and lifecycle.

| Property | HostPlugin | ProjectPlugin |
|---|---|---|
| Owner | UmbraFlow framework repository | External product/game project |
| Payload | Trusted first-party C++ compiled into one executable | Verified project-owned Luau and pure data |
| Selection | Executable-specific typed boot plan and HostProfile | Project release and `umbraflow-project.json` |
| Identity | readable id/API version and active-composition diagnostics; no hash | current ProjectRegistration and exact plugin bytes |
| Privilege | only the typed installation arguments compiled for that unit | no Host, Receipt, controller, database, clock or filesystem authority |
| Lifecycle | Host, Session, Target or Generation owner | registration, session pinning and bounded pure-data invocation |
| Extends | OCR, target binding, trace, command/UI and runtime composition | game semantics through `derive`, `plan`, `next_step`, `reconcile`, `reduce` |

The bare word `Plugin` is forbidden in new Host-side vocabulary. Use
`HostPlugin` for framework composition and reserve `ProjectPlugin` for the
external product boundary.

`modules/project` is the offline Project Kit. It publishes product project
releases; it is not itself a ProjectPlugin.

## Current facts

The current tree already has a partial product-runtime spine:

- `service::ProductLifecycle::start` is the sole production construction site
  for `OperatorCoordinator`;
- `operator_runtime::OperatorTaskHost` owns `OperatorCoordinator` and
  `task::TaskHost`, binds one controlled target and seals the
  reserve → deliver → record join;
- `ProductLifecycle` owns Project loading, RuntimeArtifact activation,
  ProjectPlugin registration, SessionManifest creation, controller binding,
  lease ownership and the observe/execute/wait/reconcile surface;
- `modules/cli/source/cli/observe.cpp` still constructs `FileTraceSink`,
  `TraceRecorder`, `EngineSession` and `TaskContext`;
- Windows target and OCR provider assembly lives in
  `modules/cli/source/cli/observe-windows.cpp` and `platform/`;
- the process command registry and dispatch live in the constexpr table in
  `entry/cli/main.cpp`;
- `explore`, `open`, `ocr` and `targets` retain separate composition paths.

The last two bullets are different seams. Provider/runtime assembly can move
without moving the command table. A future command-contribution migration must
redesign the command safety evidence explicitly.

Frame, action, OCR and trace each still have only one shipped production
provider. Provider variability alone therefore does not justify a framework.

## Low-cost correctness baseline

Two real defects are independent of HostPlugin and belong to option A.

### Explicit ProductLifecycle close

`ProductLifecycle::shutdown()` already releases the lease and is idempotent,
but production never calls it and the default destructor does not call it.

This is not literally a one-line correctness fix: after `start`, observe has
multiple error-return paths. The repair must:

- release on success and every later failure;
- preserve the primary error when cleanup also fails;
- report an otherwise standalone shutdown failure at the process boundary;
- leave a `noexcept` destructor only as last-resort cleanup;
- prove lease release and exactly-once shutdown in tests.

### Real SessionManifest identity in trace

`ProductLifecycle::start` already creates the real SessionManifest and uses its
hash to pin Operator state. `ProductIdentity` does not expose that value, so
`cli::observeProject` currently writes the RuntimeModel semantic hash into
`TraceStreamSpec::sessionManifestHash`.

Store the already-created identity in `ProductLifecycle::Impl`, expose it in
`ProductIdentity`, and pass it to trace. This does not require HostPlugin.

These defects must not be used as the cost justification for option B or C.
The incremental HostPlugin case is organization of multiple composition roots,
future front-end reuse, typed provider selection and reversible contribution
ownership.

## Simplified HostPlugin model

### Typed installation unit

A HostPlugin is a named statically linked install function with executable-
specific arguments and an explicitly owned result. It need not implement a
common virtual base.

Conceptually:

```cpp
struct HostPluginDescriptor final
{
    std::string_view id;
    uint32           apiVersion;
    HostPluginScope  scope;
};

struct WindowsTargetHostPlugin final
{
    static constexpr HostPluginDescriptor descriptor{
        .id         = "windows-target",
        .apiVersion = 1,
        .scope      = HostPluginScope::Session,
    };

    [[nodiscard]]
    static auto install(WindowsTargetApplyArgs args)
        -> Result<std::unique_ptr<InstalledWindowsTarget>>;
};
```

This is vocabulary, not an approved header. Each installed owner may remain a
concrete type. A small product aggregate owns the installed units and shuts
them down in reverse construction order.

### Dependencies are C++ parameters

The pilot has no `ServiceId`, string lookup, `Context::get`, runtime requires/
provides table or graph solver. A HostPlugin receives exactly the typed values
its install signature names.

```text
typed ProductHostProfile
  -> fixed executable boot plan
     -> install OCR provider
     -> install target provider
     -> install trace provider
     -> install authority runtime
     -> construct observe runtime
```

The boot plan owns ordering. CMake owns which interfaces and implementations
can be named. Mutual dependency is lifted into a higher typed owner, as
`OperatorTaskHost` already lifts the Operator/Task join.

This intentionally gives up DSH/Cordis reactive dependency behavior. It keeps
the organizational benefit without creating a second module system.

### Closed executable catalog

Each executable may expose a constexpr descriptor catalog for diagnostics and
safety gates. It is metadata over factories already named by the typed boot
plan; it is not a loader.

```text
Product executable
  may compile Windows live target and native action providers

Conformance executable
  must not link live target or native action providers

Project Kit executable
  contains only offline project-build behavior
```

There is no global built-in catalog shared by all executables and no discovery
of every linked provider by name. Conformance isolation remains a link-time
fact.

### Typed HostProfile

The first HostProfile is a C++ value or validated configuration decoded into a
closed value:

```cpp
struct ProductHostProfile final
{
    OcrProvider    ocr{OcrProvider::Onnx};
    TargetProvider target{TargetProvider::Windows};
    TraceProvider  trace{TraceProvider::File};
};
```

A profile may select only enum values compiled into that executable. It cannot
add a plugin, widen installation arguments, reorder the boot plan or name an
authority. The pilot applies profiles only at process/session start.

Profiles contain no implementation digest, catalog digest or composition
hash. See the separate
[hash management simplification proposal](2026-08-14-hash-management-simplification-proposal.md).

## Authority and generation realms

### Authority installation

Only `AuthorityRuntimeHostPlugin` receives `AuthorityBootArgs`. It constructs
the current `ProductLifecycle`/`OperatorTaskHost` aggregate and publishes a
narrow product surface.

Ordinary HostPlugin install signatures must never receive or return:

- `OperatorCoordinator&`;
- `task::TaskHost&`;
- `task::TaskHost::Receipt` or a Receipt-minting capability;
- an unrestricted `engine::IActionSink&` or action-sink factory.

The current reserve → deliver → record join stays sealed inside
`OperatorTaskHost`. Typed arguments are compile-time capability control; a
runtime registry is not.

### Runtime and Annotation generations

`TaskHost` already owns two non-converting generation kinds:

| Generation realm | Owns | Must not resolve |
|---|---|---|
| RuntimeGeneration | verified RuntimeArtifact, RuntimeModelBinding and production VM | authoring directory, screenshot-bearing exploration capability |
| AnnotationGeneration | authoring directory and privileged ExplorationSession | production RuntimeArtifact/Receipt authority |

If a HostPlugin participates in a Generation scope, it receives either
`RuntimeGenerationApplyArgs` or `AnnotationGenerationApplyArgs`, never a
generic Generation context. No profile binding can connect the two.

## Lifecycle and error model

The boot plan validates the typed profile before producing effects. It then
installs in its fixed order. Failure shuts down already installed owners in
reverse order; normal shutdown uses the same order.

The error rules are:

1. validation errors occur before any product effect;
2. installation returns `Result<Owner>` or, where a concrete owner cannot be
   proven nothrow-movable, `Result<std::unique_ptr<Owner>>`;
3. the original installation/business error remains primary if rollback also
   fails; rollback failures are retained as deterministic diagnostics;
4. explicit `shutdown() -> Status` is idempotent and is the only path allowed
   to report a failed quiescence boundary;
5. destructors remain `noexcept` last-resort cleanup;
6. thread stop/join stays with the component that owns the `std::jthread` or
   `stop_source`; a generic effect wrapper cannot claim quiescence for it.

The pilot does not require `std::map` or `std::set`: descriptor catalogs are
constexpr arrays/spans, profiles are closed values, and the boot plan is fixed.
The complete clang-analysis preset runs from the first kernel change, including
`bugprone-exception-escape` under `WarningsAsErrors: '*'`.

A reusable registration group is admitted later only when at least two real
command/UI/event contribution types need reversible ownership. It holds
concrete move-only tokens, not arbitrary destructor lambdas.

## Pilot HostPlugin mapping

Option B covers only this vertical slice:

| HostPlugin | Current implementation adapted | Policy |
|---|---|---|
| `authority-runtime` | `service::ProductLifecycle`, `OperatorTaskHost`, sealed ProjectPlugin registration | Host restart for pilot |
| `windows-target` | `modules/cli` target binding, WGC frame source and controller action sink | Session boundary |
| `onnx-ocr` | ONNX `IOcrEngine` provider factory | Session boundary |
| `file-trace` | `FileTraceSink` and recorder factory | Session boundary |
| `observe-runtime` | observe's EngineSession/TaskContext assembly | one Session execution |

The constexpr five-command table remains in `entry/cli/main.cpp` during option
B. The existing command safety gate therefore remains intact.

Option C may later add `open-command`, `targets-command`, `ocr-command` and
`explore-authoring`, but only after the command gate described below is
replaced by equally strong static evidence.

The following remain ordinary libraries/products:

- `core`, `domain`, `json`, `image` and `vision` primitives/algorithms;
- schema parsing and deployment validation functions;
- `EngineSession`, `TraceRecorder`, `script::Engine` and per-operation values
  created by installed factories;
- Operator and Task as the sealed internals of authority-runtime, not
  separately replaceable public services;
- conformance cases, which remain another executable link closure.

## CLI safety gate migration

`tests/test-runtime-surface.py` currently:

- extracts `Command{"name", ...}` literals from `entry/cli/main.cpp`;
- refuses retired or unreviewed commands;
- treats any of the five required commands disappearing as an error;
- pins safety-reviewed implementation paths under `modules/cli/source/cli`.

This is an intentional security control, not a snapshot to delete.

Before option C moves command registration, Stage 4 must deliver a replacement
that reads the executable-specific constexpr HostPlugin/command catalog and
proves:

- the five production commands remain mandatory in the standard product
  profile unless a separate contract change approves otherwise;
- retired and unreviewed commands cannot enter the production catalog;
- each command's capability class is explicit;
- conformance cannot link a live target/action provider;
- moving a reviewed source path cannot silently remove it from the audit set.

The command catalog remains static even if installation returns RAII
registration tokens. Runtime string discovery is not an acceptable substitute.

## Composition diagnostics, not composition identity

The running product may print or trace a readable snapshot:

```text
host profile: desktop-default
authority-runtime api=1
windows-target api=1
onnx-ocr api=1 models=<configured path/name>
file-trace api=1
observe-runtime api=1
```

This snapshot is diagnostic structured data. It is not part of SessionManifest
admission, is not hashed, and is not checked into the repository. Provider or
configuration changes are visible without making development depend on an
exact digest.

Option A still fixes the existing trace bug by carrying the real current
SessionManifest identity. That is separate from HostPlugin composition.

## Deliberate exclusions

The proposed architecture does not include:

- a stable binary plugin ABI, DLL, `dlopen` or `LoadLibrary`;
- out-of-tree C++ installation;
- a generic Context or service registry;
- runtime service graph resolution;
- Cordis reactive injection, isolate propagation, patches or HMR;
- a generic string event bus;
- automatic unload when a provider disappears;
- immediate reload across authority, Session, Target or Generation boundaries;
- replacing the ProjectPlugin system;
- checked-in profile/catalog/composition hashes;
- a requirement that every module, class or source file be a HostPlugin.

## Delivery stages and estimates

Estimates assume one senior C++ engineer familiar with the repository and
include retained tests, clang-analysis and three-platform repair.

| Stage | Outcome | Estimate |
|---|---|---:|
| 0. Correctness baseline | explicit all-path ProductLifecycle close and real SessionManifest trace identity | 2–4 person-days |
| 1. Static typed shell | vocabulary, descriptor span, typed ApplyArgs/owners, fixed boot aggregate, error tests and analysis | 3–5 person-days |
| 2. Observe vertical slice | authority-runtime, Windows target, ONNX OCR, file trace and observe-runtime | 5–8 person-days |
| 3. Typed profile/diagnostics | closed provider enums, validation and readable active composition | 4–7 person-days |
| 4. Remaining behavior and CLI gate | redesign static command evidence; migrate accepted runtime/command units | 10–18 person-days |
| 5. Hardening | failure injection, teardown stress, cross-platform gates, documentation and old-root removal | 6–10 person-days |

Option A is Stage 0. Option B is Stages 0–2, **10–17 person-days total**.
Option C is Stages 0–5, **30–52 person-days total**.

Cordis-like reactive dependency loss, hierarchical isolate contexts and HMR
remain a separate runtime project and are not estimated here.

## Acceptance

### Option A

1. Every return after ProductLifecycle start attempts explicit shutdown.
2. The primary business error and any cleanup failure have deterministic
   reporting.
3. Lease release and exactly-once shutdown are tested.
4. Trace receives the real SessionManifest identity, not RuntimeModel semantic
   identity.

### Option B

5. The boot plan is typed and fixed; no string service lookup or graph solver
   exists.
6. A partial install shuts down every installed owner once in reverse order.
7. The production executable can install the five pilot units from one typed
   profile.
8. The conformance executable still cannot link a live target/action provider.
9. Ordinary HostPlugins cannot obtain OperatorCoordinator, TaskHost, Receipt
   or unrestricted IActionSink authority.
10. RuntimeGeneration and AnnotationGeneration installation types cannot be
    converted or cross-resolved.
11. The existing constexpr command table and runtime-surface command gate remain
    unchanged during the pilot.
12. The x64/Linux/macOS analysis lanes pass from the first kernel change.
13. No profile, descriptor or SessionManifest gains a HostPlugin composition
    hash.

### Option C

14. The replacement command gate detects a missing required command, a retired
    command and an unreviewed command through deliberate mutations.
15. Active composition diagnostics name every selected provider and relevant
    readable configuration without becoming admission authority.
16. Existing CLI and conformance behavior remains unchanged except for an
    explicitly approved contract change.

## Stop conditions

Stop after option A or B and retain explicit ProductLifecycle composition if:

- the pilot exceeds 17 person-days before the five units run one real observe
  session;
- the product-local shell exceeds roughly 1,200 implementation lines before
  adapters and tests;
- most units are one-to-one wrappers with no provider choice, contribution or
  independent lifetime;
- implementing the pilot requires a public Context, runtime service graph or
  generic effect system;
- authority composition requires exposing OperatorCoordinator, TaskHost,
  Receipt or IActionSink;
- conformance begins linking a live provider;
- teardown leaves callbacks, threads, registrations or leases alive;
- developers must edit a digest when changing a profile or provider.

## Approval consequences

Approval of option B or C requires one coordinated documentation change before
C++ implementation:

1. in `ARCHITECTURE.md`'s deliberate-absence sentence, retain "no dynamic
   plugin ABI" and change the broad "no dependency-injection container" wording
   to permit only this product-local static typed-install composition while
   continuing to forbid a generic/runtime container;
2. add only the approved option's stages to the consumer repository's canonical
   execution plan;
3. add canonical `HostPlugin`/`ProjectPlugin` terminology to `CONTEXT.md`;
4. link the accepted hash ruling to the
   [hash simplification proposal](2026-08-14-hash-management-simplification-proposal.md);
5. retain this document as rationale, not a second work list.

Until those amendments land, current explicit composition and current hash
contracts remain authoritative.

## External reference boundary

The organizational inspiration is documented by the official
[DeepSeek Harness architecture](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/architecture.md)
and
[Cordis primer](https://github.com/deepseek-ai/deepseek-harness/blob/master/docs/cordis-primer.md).
Those documents explain the source pattern; this proposal defines the much
smaller contract UmbraFlow would evaluate.
