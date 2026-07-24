# UmbraFlow Whole-System Architecture Overview

This document describes the architecture that can be built and run in the repository as of
2026-07-24. [`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) is authoritative for module
dependencies; unfinished product capabilities are tracked under
[`docs/plans/`](../../plans/README.md).

## What the system does

UmbraFlow authorizes background actions from visual evidence. Platform-independent modules own
recognition and authorization, while Windows code owns target capture and input delivery. They are
combined only under `entry/`, so recognition policy does not depend on HWND and controller does not
need to understand pages or recognizers.

```text
core
  ↑
domain
  ↑
vision      image
  \          /
   annotation
       ↑
     engine

controller (Windows) -> core, domain
script               -> core, domain
entry/cli            -> engine + controller
entry/workbench      -> annotation + engine + controller + image
```

An arrow points from a dependent toward its dependency. `vision` and `image` are peers and do not
depend on one another.

| Module | Owns | Does not own |
| --- | --- | --- |
| `core` | `Result`, checked arithmetic, strong types, monotonic time, UTF-8, contract checks | Game, image, page, or platform policy |
| `domain` | Frame identity, coordinate spaces, target generations, detections, observation leases, error classification | Recognition algorithms and input delivery |
| `vision` | Gray8 conversion and resource-bounded SAD matching | PNG, page rules, and product thresholds |
| `image` | PNG encoding/decoding, pixel-layout conversion, and rectangular crops | Recognizers and action authorization |
| `annotation` | Annotation model, page recognition, evidence, authorization, and deterministic compilation | Window capture and input delivery |
| `engine` | Published-artifact loading, same-frame decisions, port orchestration, and trace events | Win32, target selection, and the Luau host |
| `controller` | Window discovery, target continuity, WGC, DPI, and strict-background input | Page recognition and action selection |
| `script` | Minimal Luau embedding | The product task runtime; sandboxing, cancellation, and quotas remain incomplete |

`controller` is the only reusable module restricted to Windows. The real adapters used by
`umbra-workbench` and `umbra-flow run` are also Windows-only, but platform code remains under
`entry/`; it does not flow back into domain, vision, image, annotation, or engine. Linux and macOS
can therefore still build the platform-independent modules, and CI can test the runtime flow with
fake ports.

## The three executables

| Entry point | Purpose | Current status |
| --- | --- | --- |
| `umbra-workbench` | Edit annotation projects, capture source images, preview, compile, and publish | A1 annotation tool |
| `umbra-flow run` | Load a published project, wait for a page, find one action, and click once | B1 C++ smoke flow |
| `m0-demo` | Verify WGC capture and strict-background input | Frozen; no longer carries product features |

These three paths must not be mixed:

- Workbench can generate recognition assets, but it has no input capability.
- `umbra-flow run` reads only the generated runtime manifest and templates, not the full authoring
  screenshots.
- `m0-demo` does not use the annotation authorization stack and cannot serve as shared
  implementation for engine or CLI.

## From an authoring project to runtime

Workbench maintains two document forms:

- `AuthoringDocument` stores the complete editable state and can be reopened for further changes.
- `RuntimeManifest` retains only what runtime recognition and authorization require.

A typical directory looks like this:

```text
project.toml
assets/sources/<content-hash>.png
annotations.toml
generated/annotations.runtime.toml
assets/templates/<content-hash>.png
```

`compileAuthoringDocument` crops templates from source images, canonically encodes the PNGs,
computes each `ContentHash` over the encoded bytes, and then creates the runtime manifest.
Workbench publishes content-addressed assets first and replaces
`generated/annotations.runtime.toml` last. Runtime trusts only assets referenced by that manifest;
it does not scan directories and guess which files to load.

Publication is not currently a cross-file transaction. If the final manifest replacement fails,
the disk may contain a new authoring document alongside the old runtime closure, but the loader
will not combine them into a half-new, half-old project.

## How one run happens

The composition root for the Windows product path is `entry/cli/run-windows.cpp`:

1. CLI parses arguments, loads the runtime project, and resolves page and action names to stable
   IDs.
2. These offline checks complete before desktop access. A corrupt manifest, missing template, or
   unknown name does not create platform resources first.
3. Controller resolves the unique target window and establishes a `TargetGeneration` and WGC
   capture session.
4. `WgcCaptureSession::capture` returns a `Frame` with pixels, capture time, coordinate transform,
   and identity.
5. `EngineSession::observe` creates an `Observation`. Page resolution and action lookup within the
   same observation always use the same frame; they do not recapture implicitly.
6. Annotation resolves the page to `ResolvedPage`, `UnknownPage`, or `AmbiguousPages`. Only one
   uniquely resolved page with complete recognition evidence can continue.
7. `authorizeCoordinateAction` checks page permission, action detection, observation lease,
   project fingerprint, and frame identity together.
8. Engine converts the action coordinate to client space, revalidates the target instance before
   delivery, and passes the original lease to `ActionSink`.
9. Controller rechecks session, generation, lease age, coordinate bounds, and the Win32 encoding
   range before delivering through `PostMessageW`. Failure never falls back to foreground or global
   input.
10. Engine creates versioned `TraceEvent` values, and CLI writes and flushes them as JSONL.

## Constraints that must remain true

### Failures do not authorize an action

The following conditions must stop before authorization or delivery:

- invalid resources, schema, hashes, reference closure, or geometry;
- a page that cannot be resolved uniquely;
- recognition that is cancelled, times out, or exhausts its comparison budget;
- a project fingerprint, target generation, or frame identity mismatch;
- an observation that has expired or has already been consumed;
- a target instance that cannot be revalidated before delivery;
- a required pre-delivery trace record that cannot be written according to contract.

An error cannot be converted into "try to click once," nor can foreground activation or global
input be used as a fallback.

The post-click `ClickDelivered` and `ObservationInvalidated` trace records can also fail to write.
Such a failure cannot undo a click that has already happened. Engine invalidates the observation
before propagating the trace error, so a caller must not interpret the failure as zero delivery and
retry the same action.

### Same-frame decisions and identity isolation

Frame identity is the tuple `(SessionId, TargetGeneration, FrameId)`:

- `FrameId` increases monotonically within one capture session;
- `TargetGeneration` advances when the target instance, window handle, client size, or continuity
  changes;
- `SessionId` separates distinct capture sessions.

`Observation` owns the original frame. Page evidence, action evidence, and the lease all come from
that same frame. After successful delivery, the observation is invalidated immediately to prevent
a duplicate click.

### Determinism and resource bounds

- Grayscale conversion, SAD, thresholds, and candidate ordering use integer rules.
- Similarity thresholds are stored as basis points in `[0, 10000]`, with an inclusive integer hit
  boundary.
- Matching order, page order, TOML field order, and JSON field order are fixed.
- File size, image dimensions, template counts, search comparisons, wait duration, and retry counts
  are bounded.
- Cancellation, timeout, and budget exhaustion preserve an explicit stop reason and cannot be
  disguised as an ordinary miss.

### Ownership and platform boundaries

`Frame` shares immutable pixel ownership, and views such as `GrayImage` are used only while their
backing buffer remains alive. `EngineSession` exclusively owns its three ports, and an
`Observation` cannot cross session boundaries. Platform handles, D3D objects, and Win32 input
implementation remain under controller or the platform directories in `entry/`.

Strict-background is a restriction on reachable APIs, not an optional switch. The allowed input
path currently ends at `PostMessageW` for the target window.
`SetForegroundWindow`, `SetFocus`, `SendInput`, `mouse_event`, `keybd_event`, and `SetCursorPos`
are all forbidden.

## Where to look

| Question | Document |
| --- | --- |
| Foundational error, numeric, ownership, and time capabilities | [`module-core.md`](module-core.md) |
| Frame identity, coordinates, detections, and leases | [`module-domain.md`](module-domain.md) |
| Gray8/SAD, PNG, and pixel layouts | [`module-vision-image.md`](module-vision-image.md) |
| Authoring documents, compilation, page recognition, and authorization | [`module-annotation.md`](module-annotation.md) |
| Runtime ports, Observation, actions, and tracing | [`module-engine.md`](module-engine.md) |
| Current Luau embedding and its security gaps | [`module-script.md`](module-script.md) |
| WGC, target continuity, DPI, and input | [`module-controller.md`](module-controller.md) |
| Editing, preview, and publication in the annotation tool | [`entry-workbench.md`](entry-workbench.md) |
| Product CLI and Windows composition | [`entry-cli.md`](entry-cli.md) |
| Frozen on-hardware acceptance path | [`entry-m0-demo.md`](entry-m0-demo.md) |

## Verification scope

Platform-independent tests cover coordinates and identity, SAD, PNG, authoring documents, page
resolution, action authorization, runtime loading, Observation lifetime, budgets, cancellation,
and trace serialization. Controller tests cover target generations, leases, message sequences,
coordinate ranges, and forbidden APIs.

Real WGC behavior, occlusion, minimization, DPI, UIPI, and foreground stability still require
on-hardware Windows verification. Synthetic tests cannot replace that evidence. Current unfinished
items are listed in [`docs/TODO.md`](../../TODO.md), and later product phases are described in
[`docs/plans/2026-07-21-product-form-and-roadmap.md`](../../plans/2026-07-21-product-form-and-roadmap.md).
