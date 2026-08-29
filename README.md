# UmbraFlow

Native C++23 visual automation framework for Windows: WGC capture, template
matching, strict background PostMessage input.

A **project** is a directory of data — schemas, a RuntimeArtifact, and Luau — that
teaches the framework one game. Nothing in this repository knows a game name.
If you are about to write that directory, this file is the orientation; the depth
is in [`docs/`](docs/INDEX.md), and the only document a consuming repository reads
is the generated [`docs/PUBLIC-CONTRACT.md`](docs/PUBLIC-CONTRACT.md).

## The Tool Runtime

Every way an actor or a project reaches the world is a **Tool call**. Observing
the screen, waiting, delivering input, recording an audit entry, and every
game-specific verb a project exposes are all the same kind of thing, admitted the
same way and recorded in the same durable ledger.

Three kinds of actor drive a game — `ControllerKind::Agent`,
`ControllerKind::Human` and `ControllerKind::Script`, a project's own Luau
automation — and they are equally first-class:

```text
Agent -------------------+
Human Workbench / CLI ---+--> one admission path --+--> framework.* Tools
Project Luau automation -+                         +--> <project>.* Tools
```

They are not three code paths. Each has a *translator* —
`AgentToolAdapter`, `HumanToolAdapter`, `ProjectAutomationAdapter` in
`modules/operator/source/operator/tool-actor-adapters.hpp` — and each translator
builds one `ToolAdmissionRequest`, the only value `OperatorCoordinator::admitToolCall`
accepts. A Project Tool handler may issue recorded child calls through that same
admission path; every child is independently checked under its durable ancestry.
An adapter can construct nothing that is executable: the call coordinate is
minted privately by the issuing seam. So a new caller cannot
acquire a second execution path — the worst mistake it can make is
translating its own transport badly, which is one class of bug rather than a
whole second authority evaluation.

That is what makes the door irrelevant. Policy, approvals, effect-envelope
intersection, session and target authority, lease, fence and budgets are
evaluated once, inside admission, and every one of them is re-read from the live
session row rather than taken from the request. A human is not privileged for
being a human, and a project's own script is not privileged for running inside
the process; what an actor may reach is its own identity, profile and capability
set.

Identity decides what an actor may *start*. A call issued **inside** a running
Tool is judged instead against that Tool's registered child-effect declaration
and the admitted root envelope, so an actor can be admitted to a high-level
project Tool without its low-level Framework child ever becoming directly
callable by that actor. Whether a call is a root or a child is derived from its
coordinate, not stated by whoever produced it.

## What a project ships: two closures

Each deployment in `umbraflow-project.json` declares exactly two Luau closures,
and **both slots are mandatory**:

```json
"reducer_closure": {
  "entry": "main",
  "exported_entry_points": ["reduce"],
  "modules": [{"name": "main", "path": "plugin/expedition-reducer.luau"}]
},
"tool_closure": {
  "entry": "main",
  "exported_entry_points": ["approval", "move", "survey", "trade"],
  "modules": [{"name": "main", "path": "plugin/expedition-tools.luau"}]
}
```

- The **reducer closure** exports exactly its `plugin_id` and `reduce`, and
  nothing else. It folds a Journal batch into project state. It compiles on
  `script::PureDataProgram`, whose resolver carries no scoped module at all, so
  it cannot observe a frame or call a Tool — that is a property of the program
  type, not a check.
- The **tool closure** exports its `plugin_id` plus one entry per Tool the
  deployment's Tool Catalog declares. `tool_bindings` states the join from Tool
  name to entry. It compiles on `script::ScopedToolProgram`.

A project with no Tools ships an explicitly empty tool closure —
`"exported_entry_points": []` — rather than omitting the slot. An absent slot
would be a second reading of the document ("absent means pure"), and this
repository refuses second readings: the loader fails with
`required has no member 'tool_closure'`.

Three sources have to agree and none is derived from another:
`exported_entry_points` is authored and hashed, `tool_bindings` names the join,
and the Tool Catalog declares the descriptors. `ProjectToolBindingTable::bind`
is the only mint, so holding one is proof that no binding names an unexported
entry, no declared Tool is unbound, and no binding covers an undeclared Tool.

## A Tool name carries its namespace

Framework owns `framework.*`. A project owns its own registered namespace, which
is its `plugin_id`. That is why the dot is required rather than decorative:

```text
framework.screen.observe        arcana.expedition.move
framework.workflow.wait         fixture.alpha.command-1
```

`validateToolNameOwnership` is the one rule — a Tool name must sit inside the
namespace its registrant owns — and it is a prefix test at a dot boundary, not a
split at the first dot, because a `plugin_id` is itself namespaced. A
registration claiming a `plugin_id` under `framework.` is refused where the
namespace is stated, so there is no separate reserved-prefix rule per Tool name.

## The `@umbraflow/` SDK

Project Luau resolves module names inside a closed graph. `@umbraflow/` is
reserved: a request under it never falls through to project modules, a
filesystem, a search path, or a network.

**Pure modules** cost nothing and reach nothing. They create no Tool
invocation, spend no budget, and are the same on every platform because their
Unicode data is pinned by the release rather than read from the host locale:

| module | what it is |
|---|---|
| `@umbraflow/text` | Unicode normalization, full case folding, trimming, splitting |
| `@umbraflow/utf8` | validation, code-point traversal, slicing, classification |
| `@umbraflow/json` | strict parsing, deterministic encoding, immutable values |
| `@umbraflow/jcs` | RFC 8785 canonicalization and canonical equality |
| `@umbraflow/collections` | deterministic list/set helpers, immutable updates |
| `@umbraflow/result` | one frozen success/error envelope vocabulary |

**The Tool face** — the modules a chunk calls Tools through exist only in a tool
closure, and there is no hand-written list of them. The pinned Tool catalog is
the single authority and the face is GENERATED from it, one module per Tool
namespace: a Tool named `<namespace>.<member>` is `<member>` on
`@umbraflow/<namespace with dots as slashes>`, with a leading `framework`
elided because `@umbraflow/` already is it.

```lua
local screen = require("@umbraflow/screen")   -- framework.screen.*
local input  = require("@umbraflow/input")    -- framework.input.*
local dream  = require("@umbraflow/chaos/dream") -- a Project's own Tools
```

Nothing per-Tool is written by hand anywhere, so a Tool added to the catalog is
callable with no source change, and a Tool removed from it stops being callable
the same way. A call takes one argument table whose keys are the input schema's
own property names, unchanged, so a chunk and an MCP client send the same
object. They are absent from the reducer's resolver, from the trusted Framework
bundle, and from every project-global whitelist, so a reducer that names one
fails to load with the module named.

A generated module's only way to *cause* anything is a Tool call, over one
synchronous native `invoke` held by a single release-owned renderer that Project
source cannot resolve. The one thing it reads without a call is the run's
pinned, read-only Tool catalog, published as `@umbraflow/catalog` — advisory
data that spends no budget and cannot make the Operator accept anything.

A call reads as an ordinary call: on success it evaluates to the Tool's own
result, and on any other terminal delivery it RAISES the whole answer envelope
as a table, so `pcall` recovers `delivery`, `call_identity` and the provider's
verbatim `error` unchanged. Nothing about a failure is discarded, and a failed
call cannot be mistaken for a success by ignoring it.

## A script is an ordinary loop

Project automation is not a state machine, a step list, or an `advance` callback.
It is Luau with `for`, `if` and locals:

```lua
local screen   = require("@umbraflow/screen")
local ui       = require("@umbraflow/ui")
local workflow = require("@umbraflow/workflow")

for _attempt = 1, attempts do
    local shot = screen.capture{}.screenshot_sha256
    local seen = screen.observe{ screenshot_sha256 = shot }
    local action = seen.observation_reference.ui_actions[1]
    if action == nil then break end

    ui.click{
        observation_reference = seen.observation_reference,
        ui_target = action.ui_target,
        binding   = action.binding,
        action    = action.action,
    }
    workflow.wait{ duration_ms = 0 }
end
```

It survives a crash without anyone writing recovery code, and without the
framework ever serializing a VM stack. `invoke` is synchronous — no yield, no
coroutine suspension, no completion callback — and every Tool call is a **durable
suspension boundary**: the call's position is recorded before the provider runs
and its outcome is recorded when the provider answers.

Resumption is therefore **deterministic restart plus call replay**. The same
closure runs again from its entry in the same pinned environment. A call whose
coordinate matches a terminal recorded row receives that recorded result and
performs no provider execution and no external delivery; only the first position
beyond the record executes. The lookup key is the ordinal coordinate alone — run,
parent, and the child index the C++ seam assigned — and a script can neither see
nor name that index.

Everything else about the call is a stored *attribute* compared field by field at
that coordinate: run identity, framework release, protocol, environment,
provider kind, registration, catalog hash, tool name, tool version, canonical
arguments, observation reference. The first mismatch is
deterministic-replay divergence: `divergedToolCallField` names the field that
diverged, and the refusal is terminal at the seam — the failure is recorded and
the VM is destroyed without resuming the script, so no `pcall` can turn a
divergence back into ordinary control flow. Folding name and arguments into the
key is deliberately forbidden: a changed argument would then miss the lookup,
look like a first new call, and execute.

The one thing replay cannot make certain is a mutating Framework provider that
was in flight when the process died. That call is classified `possible`, and it
freezes mutation on the controlled target until trusted evidence resolves it. A
project handler is not in that class: its whole effect surface is its own
recorded children, so it re-enters and replays.

## Included

- Manifest-driven CMake modules, MSVC / Linux Clang-GCC / macOS Clang presets,
  Ninja and Visual Studio 2022 workflows, optional ccache
- Warnings-as-errors, compiler hardening, clang-tidy, sanitizer and static
  analysis presets, and module-graph validation
- Rust-informed C++23 capabilities: checked arithmetic, strong domain types,
  structured results, explicit `unsafe/` boundaries, checked contiguous access,
  portable lifetime and thread-safety annotations
- Variant matching, scope cleanup, type-safe flags, lock-coupled state, and
  compile-time-validated enum names with exact bidirectional conversion
- Doctest unit tests, an exported Operator conformance binary, and a one-command
  local gate
- Reusable review, diagnosis, planning, and validation skills

## Build

On Windows, activate MSVC in the current shell first:

```bat
call .claude\skills\build-project\script\windows\build-env.bat
```

The whole gate is one command, and it takes no flags:

```bash
scripts/ci-local.ps1     # Windows
scripts/ci-local.sh      # Linux and macOS
```

It picks the host debug preset — `x64-debug`, `linux-debug`, `macos-debug` —
runs the repository checks, configures, builds, runs `ctest -L CI`, and prints
`GATE: PASS` only when every step passed. It carries both local parallelism caps
itself: the build takes half the logical processors and `ctest` runs six-wide.
Hosted CI invokes CMake and CTest directly and never this script, so its runners
stay uncapped.

Building by hand outside the gate needs those caps on the command line, because
Ninja defaults to every core and `ctest` to one test at a time — neither is what
you want on the machine you are working at:

```bash
cmake --preset x64-debug
cmake --build --preset x64-debug -j <half the logical processors>
ctest --test-dir build/x64-debug -L CI --output-on-failure --parallel 6
```

Do not put `jobs` in `CMakePresets.json`; the cap belongs on the local command
line and in `scripts/ci-local.*`. See
[`.claude/skills/build-project/SKILL.md`](.claude/skills/build-project/SKILL.md)
for the complete workflow and
[`cpp-coding/references/safety-profile.md`](.claude/skills/cpp-coding/references/safety-profile.md)
for the safety contract. AddressSanitizer and static-analysis presets are
`x64-asan` and `x64-analysis`; Linux additionally provides `linux-ubsan` and
`linux-tsan`.

## Where to read next

- [`docs/INDEX.md`](docs/INDEX.md) — the map.
- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — module ownership, dependency
  direction, and the deliberate absences.
- [`schema/`](schema/) — the normative document shapes; the files are the
  authority.
- [`examples/umbraflow`](examples/umbraflow) and
  [`examples/arcana-expedition`](examples/arcana-expedition) — two project
  directories written the way a consumer writes its own.
- [`docs/decisions/`](docs/decisions/README.md) — dated rulings, frozen.
- [`docs/pitfalls/`](docs/pitfalls/README.md) — read before investigating a
  failure.
