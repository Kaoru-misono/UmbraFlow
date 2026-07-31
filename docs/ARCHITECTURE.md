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
entry/${PROJECT_NAME}       -> core, engine, task; + controller (Windows adapters)
entry/input-agent (Windows) -> controller, trace, image (umbra-input-agent)
entry/m0-demo (Windows)     -> entry/input-agent, vision, image (frozen M0 substrate demo)
entry/workbench (Windows)   -> annotation, image (authoring backend library only)
entry/authoring (Windows)   -> entry/workbench, entry/cli, image (umbra-authoring)
domain                -> core
vision                -> core, domain
image                 -> core, domain
annotation            -> core, domain, vision, image
engine                -> core, domain, annotation
script                -> core, domain
controller (Windows)  -> core, domain
tests                 -> modules under test
```

Edges list every declared module dependency, including private ones such as
`annotation -> image`. Vendored third-party targets, such as `image_stb` and the
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
  model payloads are committed under `external/`. Its caller is the
  `umbra-input-agent` `read` verb.
- `modules/image/`: platform-free PNG I/O, pixel-layout conversion, and
  deterministic rectangular cropping; vendored codecs stay behind its FFI boundary.
- `modules/annotation/`: platform-free annotation catalog validation, page
  resolution evidence, action-authorization contracts, canonical authoring and
  runtime documents, and deterministic template compilation.
- `modules/engine/`: platform-free automation engine — capture/input/trace
  ports, the runtime manifest loader, a versioned JSONL trace vocabulary, and
  the Observation-handle session API (observe once, query the same frame, and
  any coordinate action authorizes, delivers with lease fencing, and
  invalidates the observation).
- `modules/controller/`: Windows-only discovery, target lifecycle, Windows
  Graphics Capture sessions, and strict-background input.
- `entry/`: executable targets and composition roots. `cli/` is the product
  entry `umbra-flow`; its `run` subcommand composes engine ports over the
  controller (WGC frame source, lease-forwarding click sink, JSONL trace).
  `input-agent/` is the `umbra-input-agent` annotation front-end: it serves one
  authoring session's command queue against a raw window, and it is the third
  `trace::FrontEnd`. Its `read` verb is `modules/ocr`'s composition root: it
  resolves the model payload beside the executable and brings the engine up on
  first use, so a session that only captures pays nothing and a missing payload
  fails one command rather than the launch. It also owns the entry-level
  substrate it shares with the demo below -- frame PNG output, path
  confinement, target selection and capture-session setup, JSON string
  escaping, error text, and the command-line parsing primitives. `m0-demo/` is the frozen M0 substrate demo
  kept as the real-machine acceptance reference: the fixed
  home -> result -> reset loop and the `capture` diagnostic, and nothing else
  since the front-end left it on 2026-07-31. The demo links the front-end's
  library and never the reverse, so retiring the demo is a delete rather than
  another extraction. The Windows-only `workbench/` is the annotation
  authoring backend library (`${PROJECT_NAME}_workbench_support`): the editing
  layer, the falsification matrix in `preview.*`, source ingestion, and
  publication of validated authoring projects through a narrow platform
  file-publication boundary; content-addressed assets precede the runtime
  manifest commit point. `authoring/` is the `umbra-authoring` development
  tool, the only way to author a project; it drives that library so every
  write goes through `AuthoringDocument`'s validation.

  > Corrected 2026-07-31: `workbench/` hosted the `umbra-workbench` GUI (Dear
  > ImGui + D3D11) until `b57b67b` archived it — the panels, the window shell,
  > the texture cache, the file dialog, the one-shot capture source, and the
  > imgui submodule. Only the backend below that line remains, because
  > `umbra-authoring` already linked it. Git history holds the GUI. Deciding
  > artifact:
  > [capability model plan](plans/2026-07-31-annotation-model-capabilities.md)
  > §四之二.1.
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
