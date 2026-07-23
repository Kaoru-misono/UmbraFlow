# Architecture

The project uses April2's manifest-driven CMake module loader. Each direct child
of `modules/` with a `manifest.txt` becomes a CMake library target named
`${PROJECT_NAME}_<module>`.

```text
entry/${PROJECT_NAME} -> core
entry/workbench (Windows) -> annotation
domain                -> core
vision                -> core, domain
image                 -> core, domain
annotation            -> core, domain, vision, image
script                -> core, domain
controller (Windows)  -> core, domain
tests                 -> modules under test
```

Edges list every module dependency that `scripts/check_modules.py` validates,
including private ones such as `annotation -> image`. Vendored third-party
targets declared privately by a manifest, such as `image_stb` and the Luau
libraries, are omitted.

The module graph must remain acyclic. `core` is the platform-free leaf and may
not declare link dependencies. `scripts/check_modules.py` enforces both rules.

## Repository layout

- `modules/`: reusable libraries with manifest-declared dependencies.
- `modules/domain/`: platform-free UmbraFlow frames, coordinates, detections,
  identifiers, leases, and automation errors.
- `modules/vision/`: platform-free grayscale conversion and SAD template matching.
- `modules/image/`: platform-free PNG I/O, pixel-layout conversion, and
  deterministic rectangular cropping; vendored codecs stay behind its FFI boundary.
- `modules/annotation/`: platform-free annotation catalog validation, page
  resolution evidence, action-authorization contracts, canonical authoring and
  runtime documents, and deterministic template compilation.
- `modules/controller/`: Windows-only discovery, target lifecycle, and
  strict-background input, with capture added in a later slice.
- `entry/`: executable targets and composition roots. Its Windows-only
  `workbench/` support publishes validated authoring projects through a narrow
  platform file-publication boundary; content-addressed assets precede the
  runtime manifest commit point.
- `tests/`: deterministic offline tests.
- `cmake/`: module loading, platform selection, caching, warnings, hardening,
  sanitizers, and static-analysis policy.
- `scripts/`: formatting and local CI tools.
- `.claude/skills/`: repository-local engineering workflows.

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
