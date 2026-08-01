# Architecture

The project uses April2's manifest-driven CMake module loader. Each direct child
of `modules/` with a `manifest.txt` becomes a CMake library target named
`${PROJECT_NAME}_<module>`.

The repository-root `manifest.txt` is the separate application manifest. Its
`[application]` section is the canonical source for the CMake project name and
application version; CMake generates an entry-only `application-info.hpp` from those
values. Module manifests continue to own reusable-library metadata and versions
independently. This boundary was fixed by developer decision on 2026-07-28.

```text
entry/${PROJECT_NAME}       -> core, engine, task, image; + controller (Windows adapters)
domain                -> core
vision                -> core, domain, image
image                 -> core, domain
ocr                   -> core, domain, vision
trace                 -> core, domain, vision
engine                -> core, domain, ocr, trace, vision
script                -> core, domain
task                  -> core, domain, engine, script, trace, image
controller (Windows)  -> core, domain
tests                 -> modules under test
```

This is the shape
[script-owned page model](plans/2026-07-31-script-owned-page-model.md) §三 drew:
C++ holds pixels, tickets and guarantees, and nothing else knows what a page is.
`modules/annotation` is gone (2026-08-01) along with the authoring binaries it
served; element, page, reference, appearance and edge are trusted-Luau types in
`modules/task/runtime/`, persisted to `page-model.toml`. One binary survives,
`umbra-flow`, with four subcommands: `run` a task, `drive` it by hand, `explore`
it as an Agent, `check` the falsification matrix.

Edges list every declared module dependency, including private ones such as
`vision -> image`. Vendored third-party targets, such as `image_stb` and the
Luau libraries, are omitted. (Corrected 2026-07-31: Dear ImGui was named here
until the workbench GUI was archived in `b57b67b`; the submodule is gone from
this branch.) `scripts/check_modules.py`
enforces the structural rules below (acyclicity, `core` as a leaf, manifest
shape); the edge list itself is maintained by hand and reviewed, not
machine-checked.

The module graph must remain acyclic. `core` is the platform-free leaf and may
not declare link dependencies. `scripts/check_modules.py` enforces both rules.

## Repository layout

- `modules/`: reusable libraries with manifest-declared dependencies.
- `manifest.txt`: application name and version consumed by the top-level CMake
  configuration and generated entry metadata.
- `modules/domain/`: platform-free UmbraFlow frames, coordinates, detections,
  identifiers, leases, and automation errors.
- `modules/vision/`: platform-free grayscale conversion and SAD template matching.
- `modules/ocr/`: a rectangle of pixels in, lines of text with confidences out.
  The `IOcrEngine` port and its `TextLine` vocabulary are platform-free; the
  PP-OCRv6_small adapter and ONNX Runtime stay behind its FFI boundary, and its
  model payloads are committed under `external/`. Two layouts, and the caller
  chooses: `SingleLine` recognises a rectangle the caller drew, and `Block` runs
  the detection model over a region first and reports every line it found with
  that line's own rectangle in the image's coordinates -- which is what a region
  whose contents move (a grid that scrolls continuously) needs, because there no
  rectangle per name is a model of anything. Its caller is `umbra-flow`'s
  `--ocr-models` binding (`entry/cli/platform/ocr-engine-binding.*`), shared by
  all four subcommands so both read verbs run under the same guarantees on
  every one -- including `check`, whose falsification matrix measures a cell no
  template can answer by reading the region and comparing the engine's
  confidence against the element's own floor.
- `modules/image/`: platform-free PNG I/O, pixel-layout conversion, and
  deterministic rectangular cropping; vendored codecs stay behind its FFI boundary.
- `modules/trace/`: the single JSONL evidence stream every layer writes into,
  schema `umbraflow-trace/v2` — `TraceRecorder` stamps a run's sequence, run
  id, generation id, wall clock and open framework-step scope onto every event
  before a sink sees it; `TraceStreamValidator` enforces the stream protocol,
  including which stages may request a `framework.*` event; `FrontEnd` records
  which front-end -- a project task, an operator, or the agent behind
  `umbra-flow explore` (`trace::FrontEnd::Annotation`) -- drove the run; and
  `FileTraceSink` appends one line per event and flushes after each write.
- `modules/engine/`: platform-free automation engine — the `IFrameSource` and
  `IActionSink` ports over one bound capture target, the runtime manifest
  loader, and the Observation-handle session API (observe once, query the same
  frame, and any coordinate action authorizes, delivers with lease and
  target-generation fencing, and invalidates the observation). It borrows its
  caller's `trace::TraceRecorder` rather than owning a trace vocabulary of its
  own; the vocabulary itself now lives in `modules/trace/`.
- `modules/script/`: the sandboxed Luau VM boundary — `script::Engine` owns
  one embedded VM per task generation over a whitelisted-stdlib project
  environment; the host-table and private-capability installer seams let a
  caller hang tables on that sandbox before it freezes, and a raised-error
  classifier seam lets it turn an uncaught script value into its own
  automation-error vocabulary rather than a bare `InvalidResource`. Luau
  itself stays behind an FFI boundary this header never names.
- `modules/task/`: the trusted Luau task layer — `TaskHost` owns one run's
  whole lifecycle (project and generation identity, the trace recorder, the
  engine session, the task context and the VM) so the same run is reachable
  from a CLI, a resident host, or a test; the capability surface exposes a
  project's recognition catalog to scripts as `uf.elements.*` / `uf.pages.*`
  handles and gates every engine call behind an observation-cycle ledger
  (`cycle_open` / `cycle_find` / `cycle_click` / `cycle_close`); and
  `OperatorSession` is the same capability surface's front-end for a driver
  sending commands from outside the process. The framework a project script
  runs on is itself trusted Luau rather than C++: `modules/task/runtime/`
  (`ctx`, `task`, `evidence`, `hits`, `mint`, `model`, `navigation`, `observe`,
  `oracle`, `project`, `reading`, `recognition`, `regress`, `explore`,
  `scribe`) is a set of framework modules
  compiled into the binary as a bundle, holding every policy opinion C++
  deliberately does not -- step/retry/wait, the element and page vocabulary,
  the page graph, the project file's own TOML format, and the falsification
  matrix that regresses every mark against every screen -- while C++ keeps
  only the guarantees a script must not be trusted with. `page-model.toml` is
  that project file, read and written end to end by the framework's own
  `project` module; C++ reads only its geometry fingerprint and its element
  and page names, through `readPageModelFacts`, before a VM exists. `explore`
  and `scribe` publish only into the exploration environment
  `TaskHost::startExplorationSession` boots -- the wider private surface
  (`cycle_crop`, `probe`) and the agent's own authoring loop -- which is what
  makes `ExplorationSession`, run one Luau chunk at a time by `umbra-flow
  explore`, the third front-end rather than a mode of the other two.
  `evidence` publishes into neither, for the reason `mint` does not and a
  stronger one: it is the ledger of which hits and receipts the framework minted
  and on which frame, so a project able to name it could mint a hit claiming
  interact on an element no page ever authorised.
- `modules/controller/`: Windows-only discovery, target lifecycle, Windows
  Graphics Capture sessions, and strict-background input.
- `entry/`: executable targets and composition roots. `cli/` is the sole
  binary, `umbra-flow`, and the composition root for its four subcommands, one
  front-end each: `run` composes engine ports over the controller (WGC frame
  source, lease-forwarding click sink, JSONL trace) and drives a project task
  through `task::TaskHost`; `drive` binds a target the same way and hands the
  ports to an `OperatorSession`, driven by commands appended to a queue file;
  `explore` binds a target the same way again and hands the ports to an
  `ExplorationSession`, driven by Luau chunks appended to a queue file, and
  lays out the directories a session writes into first
  (`project-skeleton.*`) because `task::ProjectFileStore` refuses a name whose
  parent does not exist and nothing below the CLI may create one; and
  `check` binds no target at all -- its frames come from
  `<project>/assets/screens` through `FileFrameSource`, and it runs the
  falsification matrix as a trusted framework routine through
  `TaskHost::runFrameworkRoutine`, which makes it the one subcommand that
  builds and runs on every host. It measures two kinds of cell: a template
  distance, and what a region reads when the element has no template, judged
  against the text the project file claims for that screen and the element's
  read floor -- so a project holding a text claim needs `--ocr-models`, and a
  check started without one is refused by name before the first screen rather
  than reporting those cells as unmeasured. Its per-cycle read budget is taken
  from the project rather than from the default that bounds a wait loop: the
  matrix reads each element at most once per screen, so the ceiling that
  follows is the project's own element count, and the routine compares it
  against the widest screen's claims before the first capture.
  Every subcommand's arguments are parsed by
  the primitives in `args.hpp`, and every failure is rendered by the same
  `formatRunError`; `drive` and `explore` additionally speak one JSON-line
  result protocol, escaped by `json-text.*`. `run`, `drive` and `explore`
  share the substrate that binds a live target: picking the one capturable
  candidate whose title matches the selector (`candidate-selection.*`), and
  the Windows-only `platform/` composition that resolves it into the two
  engine ports and a live fingerprint (`target-binding.*`,
  `wgc-frame-source.hpp`, `controller-action-sink.*`,
  `windows-target-geometry.*`), installs the Ctrl-C handler
  (`windows-console-cancellation.*`); on a non-Windows host a
  `*-unsupported.cpp` reports each of those three subcommands as unsupported,
  while `check` still builds and runs. The `--ocr-models` engine the text reads
  run on is bound by `platform/ocr-engine-binding.*`, which is declared outside
  that block because `check` needs it too and defined inside it plus an
  `-unsupported.cpp` sibling -- so elsewhere an absent flag still yields the null
  engine every subcommand handles and a supplied one is refused by name.
- `tests/`: deterministic offline tests.
- `cmake/`: module loading, platform selection, caching, warnings, hardening,
  sanitizers, and static-analysis policy.
- `scripts/`: formatting and local CI tools.
- `.claude/skills/`: repository-local engineering workflows.

## Application manifest

The root manifest uses the same section-based syntax as module manifests:

```ini
[application]
name = UmbraFlow
version = 0.1.0
```

CMake parses it before `project()`, then configures
`build/<preset>/entry/generated/application-info.hpp` for the CLI executable.
Application display metadata does not enter `core`, and it does not become a
reusable C++ module.

## Module manifest

A minimal module uses:

```ini
[module]
name = example
type = static
version = 0.1.0

[dependencies]
public = core
```

Windows-only modules add `platforms = windows` under `[module]`. The loader
omits their CMake target on every other host, and consumers gate optional tests
or composition code on that target's existence.

Place sources under `modules/example/source/example/`. Same-module includes use
quotes; other project modules use angle brackets. Keep dependency direction
acyclic and put platform-specific types behind their owning module.

## Core capability kernel

`core` intentionally contains mechanisms rather than product policy:

- `error/`: structured recoverable failures and release-safe contracts.
- `control/`: explicit early-exit sum types for visitors and traversals.
- `concurrency/`: lock-coupled mutable state without exposed storage.
- `numeric/`: checked integer arithmetic and checked conversions.
- `safety/`: portable analysis annotations and checked contiguous access.
- `text/`: UTF-8 validation and Unicode scalar encoding.
- `types/`: project integer vocabulary, strongly typed values, identifiers,
  non-zero values, flags, compile-time enum names, and non-wrapping generations.
- `time/`: monotonic process-local instants that are not serialization types.
- `utility/`: narrowly scoped variant matching and deterministic cleanup.

There is no aggregate `core.hpp`. Include the exact facility required by the
caller. Header-only facilities remain header-only; each implementation file has
a matching header.

The admission criteria and product-level candidates are documented in
the [`evaluate-core-capability`](../.claude/skills/evaluate-core-capability/SKILL.md)
skill.

## Unsafe boundaries

Normal project modules use values, RAII, checked indexing, and safe casts.
Operations that require raw allocation, pointer reinterpretation, or foreign API
lifetime proofs belong under an `unsafe/`, `platform/`, or `ffi/` directory.
Each dangerous operation requires a nearby `// SAFETY:` comment, and the boundary
must return an owning or otherwise safe abstraction to its caller.
