# UmbraFlow Whole-System Architecture Overview

This document describes the architecture that can be built and run in the repository as of
2026-07-24. [`docs/ARCHITECTURE.md`](../../ARCHITECTURE.md) is authoritative for module
dependencies; unfinished product capabilities are tracked under
[`docs/plans/`](../../plans/README.md).

## What the system does

UmbraFlow authorizes background actions from visual evidence. Platform-independent modules own
recognition and authorization, while Windows code owns target capture and input delivery. They are
combined only under `entry/`, so recognition policy does not depend on HWND and controller does not
need to understand pages or elements.

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
entry/cli            -> task (+ engine, controller)
entry/authoring      -> entry/workbench + entry/cli (+ image)
entry/workbench      -> annotation (+ image)
```

> Corrected 2026-07-31: `entry/workbench` also read `engine` and `controller`
> while it was a GUI. `b57b67b` archived that shell; what remains is the
> authoring backend library, and it links `annotation` publicly and `image`
> privately. The engine still reaches the authoring flow, but through
> `entry/cli`, because `umbra-authoring match` runs a real `RecognitionRuntime`.

An arrow points from a dependent toward its dependency. `vision` and `image` are peers and do not
depend on one another. `modules/task` and `modules/trace` are not drawn here and have no page of
their own yet; see `README.md` for the scope those two pages are still owed.

| Module | Owns | Does not own |
| --- | --- | --- |
| `core` | `Result`, checked arithmetic, strong types, monotonic time, UTF-8, contract checks | Game, image, page, or platform policy |
| `domain` | Frame identity, coordinate spaces, target generations, detections, observation leases, error classification | Recognition algorithms and input delivery |
| `vision` | Gray8 conversion and resource-bounded SAD matching | PNG, page rules, and product thresholds |
| `image` | PNG encoding/decoding, pixel-layout conversion, and rectangular crops | Elements and action authorization |
| `annotation` | Annotation model, page recognition, evidence, authorization, and deterministic compilation | Window capture and input delivery |
| `engine` | Published-artifact loading, same-frame decisions, port orchestration, and trace events | Win32, target selection, and the Luau host |
| `controller` | Window discovery, target continuity, WGC, DPI, and strict-background input | Page recognition and action selection |
| `script` | The Luau substrate: VM, sandbox, quotas, instruction and time budgets, interrupt cancellation, and the two-environment split | Task policy — waiting, retry, steps, and interrupts, which live in the Luau framework under `modules/task/runtime/` |

`controller` is the only reusable module restricted to Windows. The real adapters used by
`umbra-flow run` are also Windows-only, but platform code remains under
`entry/`; it does not flow back into domain, vision, image, annotation, or engine. Linux and macOS
can therefore still build the platform-independent modules, and CI can test the runtime flow with
fake ports.

## The three binaries, four entry points

`umbra-flow` is one binary with two subcommands; the other two each have one.

| Entry point | Purpose | Current status |
| --- | --- | --- |
| `umbra-authoring` | Author annotation projects from a command line, plus measure frames | The only authoring tool (2026-07-30) |
| `umbra-flow run` | Load a published project and run the Luau task named by `--task NAME` | P0 single-task runner |
| `umbra-flow drive` | Load the same project and execute operator JSON-line commands from `--queue` | P0 operator front-end (2026-07-30) |
| `umbra-input-agent` | Serve an annotation session's command queue against a raw window | The annotation front-end; left `m0-demo` on 2026-07-31 |
| `m0-demo` | Verify WGC capture and strict-background input | Frozen; the fixed loop and the `capture` diagnostic, nothing else |

> Corrected 2026-07-31: a fifth entry point, the `umbra-workbench` GUI, stood at
> the top of this table as "A1 annotation tool". `b57b67b` archived it; git
> history holds the shell. Its backend is still linked, now by `umbra-authoring`,
> so the authoring capabilities described further down did not go with it — but
> three affordances that existed only in the GUI did, and they are tracked in
> [the capability plan](../../plans/2026-07-31-annotation-model-capabilities.md)
> §四之二.1. Two of them have since landed as `umbra-authoring page reference`.

These paths must not be mixed:

- `umbra-authoring` can generate recognition assets, but it has no input
  capability.
- `umbra-flow` reads only the generated runtime manifest and templates, not the full authoring
  screenshots.
- **`run` and `drive` are two front-ends over one capability surface, and one generation admits
  exactly one of them.** `TaskHost` latches the first front-end to reach a generation and refuses
  the other for its life. Neither can reach anything the other cannot: the operator front-end binds
  to the same private primitives the trusted Luau framework binds to, and inherits the same
  refusals. It is a sibling consumer, not a hole into Luau — no chunk, no source, no string that
  becomes code.
- **`trace::FrontEnd` has a third value, and it is not a third consumer of that surface**
  (2026-07-31). `annotation` is `umbra-input-agent`: an authoring session driving a raw window
  to measure it, with no project, no generation and no capability surface. It is named in the same
  enum because "who drove this target" is one question with one set of answers, and an annotation
  session's clicks and captures were otherwise unattributable after the fact. It writes no
  `umbraflow-trace/v2` line — every line of that schema carries a `runId` and a `generationId`, and
  it has neither — so it stamps its own results file with the same value under the same spelling.
- `m0-demo` does not use the annotation authorization stack and cannot serve as shared
  implementation for engine or CLI.

`umbra-authoring` is a **development tool and writes nothing directly**: every change goes through
`annotation::AuthoringDocument`, because annotation output is click-authorization evidence and that
validation must not be bypassed. Its subcommands are `project init|show|save`, `page
create|add|reference`, `match ROOT ELEMENT --frame PNG [--page PAGE]` (verify an element against a
held-out screenshot), and `frames stability|probe|census` (the vision measurement primitives). One
JSON document goes to stdout per invocation, success or failure. The `match` subcommand is the
point: annotate, verify, iterate, with no human in the loop.

> Corrected 2026-07-31: the verb list read `page create|add-anchor|add-target`.
> The three drawing verbs collapsed into one — `page add ROOT PAGE NAME
> --capability C... <draw>`, where `C` is `identify[:required|:forbidden]`,
> `interact`, or `read`, given once per capability — because a capability is now
> a set rather than a choice, so the element that both names its page and can be
> clicked is one element matched once per cycle. `--shared` retired with the
> `bool shared` field. `page reference ROOT PAGE ELEMENT [--capability C...]
> [--search-roi x,y,w,h]` is new: it puts an element the project already holds
> onto a second page, which is the verb that had no CLI form at all. Its
> `--capability` takes the same `C` vocabulary and says what THIS page exercises
> on the borrowed element, so a second page can take an existing mark into its
> own signature as `identify:required` or `identify:forbidden`; omitting the flag
> inherits interact and read, never identify. Deciding artifact:
> [the capability plan](../../plans/2026-07-31-annotation-model-capabilities.md).

A failure document answers with `kind` and `response`, both in the **wire spelling** every other
JSON surface uses — `automationErrorWireName` and `failureResponseWireName`, so
`recognition_incomplete` rather than the C++ enumerator name. `response` exists so a caller can tell
a did-not-finish from a hard failure without parsing the message. Until 2026-07-30 (`81ba61b`)
`kind` used the enumerator name while `response` beside it already used the wire name, so one JSON
object answered in two conventions and an agent reading two surfaces carried two spellings of one
kind.

## From an authoring project to runtime

The authoring tool maintains two document forms:

- `AuthoringDocument` stores the complete editable state and can be reopened for further changes.
  Its schema is `umbraflow-authoring/v4`.
- `RuntimeManifest` retains only what runtime recognition and authorization require. Its schema is
  `umbraflow-annotations/v3`.

> Corrected 2026-07-31: both schema ids were bumped in one atomic change when the
> three-way annotation type became a capability set, and neither old id has a
> read path — an old schema string fails with the ordinary unsupported-schema
> error. See
> [the capability plan](../../plans/2026-07-31-annotation-model-capabilities.md) §三.

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
The authoring tool publishes content-addressed assets first and replaces
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
5. `EngineSession::observe` creates an `Observation`.
   `EngineSession::resolvePage(observation)` and
   `EngineSession::findAction(observation, pageId, elementId)` always
   use the frame held by that same observation; they do not recapture implicitly.

   > Corrected 2026-07-31: `findAction` took `(observation, id)`. It now names a
   > page, because the per-page facts moved onto the reference row — a refined
   > search region and a pinned appearance both belong to one page's use of an
   > element, and a page that does not exercise `interact` on it has no action
   > there to locate. It still authorizes nothing; the page selects a reference
   > row rather than granting one. The id type is `annotation::ElementId`; it was
   > `RecognizerId` until the same change.
6. Annotation resolves the page to `ResolvedPage`, `UnknownPage`, or `AmbiguousPages`. Only one
   uniquely resolved page with complete recognition evidence can continue.
7. `authorizeCoordinateAction` checks page permission, action detection, observation lease,
   project fingerprint, and frame identity together.
8. Engine converts the action coordinate to client space, revalidates the target instance before
   delivery, and passes the original lease to `IActionSink`.
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

Frame identity is the tuple `(CaptureSessionId, TargetGeneration, FrameId)`:

- `FrameId` increases monotonically within one capture session;
- `TargetGeneration` advances when the target instance, window handle, client size, or continuity
  changes;
- `CaptureSessionId` separates distinct capture sessions.

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
`Observation` cannot cross session boundaries. The observation does not borrow the session; a
private shared identity token follows a moved session and preserves the boundary without a raw
back-pointer. Platform handles, D3D objects, and Win32 input implementation remain under controller
or the platform directories in `entry/`.

Strict-background is a restriction on reachable APIs, not an optional switch. The allowed input
path currently ends at `PostMessageW` for the target window, for mouse messages and — since
2026-07-30 — for key messages. `SetForegroundWindow`, `SetFocus`, `SendInput`, `mouse_event`,
`keybd_event`, and `SetCursorPos` are all forbidden; a keystroke takes the `PostMessageW` route like
everything else, with the same audit record and no hold between down and up.

A keystroke is authorized differently from a click, on purpose: `IActionSink::click` takes an
`ObservationLease` and `IActionSink::pressKey` takes a `TargetGeneration`, because a lease fences a
coordinate and a keystroke names none. It still requires an open observation cycle and spends it,
because a delivered keystroke changes the screen exactly as a click does.

## Where to look

| Question | Document |
| --- | --- |
| Foundational error, numeric, ownership, and time capabilities | [`module-core.md`](module-core.md) |
| Frame identity, coordinates, detections, and leases | [`module-domain.md`](module-domain.md) |
| Gray8/SAD, PNG, and pixel layouts | [`module-vision-image.md`](module-vision-image.md) |
| Authoring documents, compilation, page recognition, and authorization | [`module-annotation.md`](module-annotation.md) |
| Runtime ports, Observation, actions, and tracing | [`module-engine.md`](module-engine.md) |
| The Luau substrate: sandbox, budgets, cancellation, and the two environments | [`module-script.md`](module-script.md) |
| WGC, target continuity, DPI, and input | [`module-controller.md`](module-controller.md) |
| Editing, preview, and publication in the annotation tool | [`entry-workbench.md`](entry-workbench.md) |
| Colour keys, the template mask, and the masked matcher | [`module-annotation.md`](module-annotation.md), [`module-vision-image.md`](module-vision-image.md) |
| Product CLI, the operator `drive` protocol, and Windows composition | [`entry-cli.md`](entry-cli.md) |
| Serving an annotation session's queue against a raw window | [`entry-input-agent.md`](entry-input-agent.md) |
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
