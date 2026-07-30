# entry/workbench Architecture Knowledge

> **DIRTY (2026-07-26)**: This document predates the page-centric refactor
> (the EditPage/PageView handle layer, authoring schema v2's Element+placement
> model, the v1 sunset, and per-placement runtime manifest expansion). Trust
> the code and `docs/plans/2026-07-26-page-centric-authoring.md` until resynced.

`umbra-workbench` is the A1 Windows annotation tool. Its GUI, capture, and file-publication
capabilities are organized around the editing model, compiler, and recognition interfaces already
provided by `modules/annotation`; it does not define another schema. It is the composition point for
these modules, while product rules remain in `annotation`, `image`, `controller`, or runtime.

## What Workbench Owns

The Workbench owns the author workflow that runs from "obtaining a complete source image" to
"publishing a generated closure that the runtime can consume":

- construct a canonical source with provenance from a PNG or a WGC frame;
- persist session state such as source selection, canvas view, and the live Preview;
- turn every modification produced by a widget into a complete, verifiable `AuthoringDocument`
  version, with undo/redo;
- display and edit recognizers, pages, regressions, `template_rect`, `search_roi`, thresholds, and
  click offsets;
- compile template assets and the runtime manifest from the current in-memory sources;
- run a bounded Preview over the selected source with the same `RecognitionRuntime` used in
  production;
- publish the project in an order that puts content-addressed assets first and the runtime manifest
  last.

It deliberately does not own the following semantics:

- schema, reference closure, rectangle, and fingerprint validity belong to
  `modules/annotation/source/annotation/authoring-document.hpp`;
- template crop, runtime manifest generation, and canonical ordering belong to
  `modules/annotation/source/annotation/authoring-compiler.hpp`;
- SAD matching, page resolution, Unknown/Ambiguous, and stop reasons belong to
  `modules/annotation/source/annotation/recognition-runtime.hpp` and
  `modules/vision/source/vision/sad.hpp`;
- the PNG codec, pixel-layout conversion, and deterministic crop belong to
  `modules/image/source/image/png.hpp` and `modules/image/source/image/pixels.hpp`;
- target discovery and the WGC session belong to
  `modules/controller/source/controller/discovery.hpp` and
  `modules/controller/source/controller/capture.hpp`;
- observation lease, action authorization, input delivery, and trace belong to the
  runtime/engine/controller chain. The Workbench has no click sink and cannot turn Preview evidence
  into an input capability.

For this reason, `entry/workbench` must not become a new domain module. In particular, do not
duplicate validation, matcher, or serialization rules inside a panel; author input must sink into
the existing canonical factory/compiler, and the panel is only responsible for surfacing failures
to the author.

The build structure is given in `entry/CMakeLists.txt`. `umbraflow_workbench_support` holds the
ImGui-free authoring backend, Preview, canvas math, and app state so that `test-workbench` can link
them directly; only the `umbra-workbench` executable adds `panels.cpp`, the Win32/D3D11 shell, the
file dialog, the texture cache, and vendored Dear ImGui.
`windows-file-publication.cpp` still lives in the support target, and WGC ingestion is also added
conditionally on the `controller` target, so this support target is "ImGui-free" rather than an
already cross-platform, formal module.

The GUI can be navigated in three layers:

1. Platform shell: the `GuiShell` in `entry/workbench/platform/windows-gui-shell.hpp` owns the Win32
   window, the DXGI swap chain, the D3D11 device, and the ImGui context; `TextureCache`, the file
   dialog, the WGC adapter, and the publication adapter also stay in `entry/workbench/platform/`.
2. ImGui-free app/backend: `AuthoringEditHistory`, `AppState`, ingestion, persistence, Preview, and
   canvas math handle only project values, `Result`, and owned bytes, and never call ImGui.
3. Panels: `entry/workbench/app/panels.hpp` exposes `drawWorkbench`, `PanelUiState`, and
   `WorkbenchServices`; `panels.cpp` translates the four immediate-mode panels into app method
   calls. The OS picker, capture, and texture upload enter only through call-scoped service
   callbacks.

`entry/workbench/app/main.cpp` is the composition root for all three layers: it parses the optional
project root and `--smoke`, loads or creates the `AppState`, creates the `GuiShell`, binds the three
`WorkbenchServices` callbacks, and then lets the shell call `drawWorkbench` synchronously every
frame. Failures are written to `stderr` only at the `dispatch`/`main` boundary.

## Editing and Publication Flow

### Editing State and Mutation Entry Points

`entry/workbench/authoring-edit.hpp` defines the editable transport: `AuthoringDraft` aggregates
`EditableSource`, `EditableRecognizer`, `EditablePage`, and `EditableRegression`. These types let a
widget temporarily write plain strings, integers, and vectors, but they are not the persistable
truth.

`makeAuthoringDraft` expands the canonical `annotation::AuthoringDocument` into the transport above;
`buildAuthoringDocument` runs the reverse, calling `AuthoringSource::create`, `ResourceName::create`,
`SimilarityThreshold::create`, `TemplateOffset::create`, `RecognizerDefinition::create`,
`PageSignature::create`, and finally `AuthoringDocument::create`.
This rebuild path explains why the GUI never does "mutate half an object now, validate later": an
edit either yields a complete, valid document or leaves the original version untouched.

`AuthoringEditHistory::apply` performs the rebuild first, then compares the canonical serialization
of the new and old documents. An identical draft returns `false` and does not pollute the history; a
genuine change moves the current document into undo, clears redo, and caps undo at
`k_maximumAuthoringUndoEntries == 100`.
`undo`/`redo` move a complete document value, so references spanning recognizers/pages are always
restored as one and the same version.

The `AppState` in `entry/workbench/workbench-app.hpp` is the ImGui-free session aggregate behind
the window:

- `m_history` is the single source of truth for the document;
- `m_sources` is the immutable source-asset cache keyed by `SourceId`;
- selection, `CanvasView`, the last Preview, and the dirty flag are transient UI state;
- `applyEdit` is the single entry point for ordinary document mutation;
- `addIngestedSource` routes both a document edit and an asset-cache update;
- `compilerSourceAssets` copies cache entries in the source order of the current document.

The cache deliberately does not roll back with undo. After undoing an import, the PNG becomes a
harmless orphan; redo can reuse it directly without re-decoding. `compilerSourceAssets` emits only
the entries referenced by the current document, so the compiler/Preview never sees an orphan; if a
document source is missing from the cache, it returns `InternalInvariant` rather than silently
compiling one image short.
Only after a successful save does `markSaved` call `pruneSourceCacheToDocument` to clear unreachable
entries.

Any committed edit, undo/redo, or source-selection change clears the stale Preview. The dirty flag,
by contrast, is conservative: undoing back to already-saved content may still stay dirty, because
the history has no restorable revision cursor; the `TODO(cpp-debt)` in the code already spells out
the upgrade direction.

### ResourceId minting

The `mintResourceId` in `entry/workbench/workbench-app.cpp` fills 16 bytes with
`std::random_device`, then sets the UUID version 4 nibble and the RFC 4122 variant bits, and finally
calls the `ResourceId::fromBytes` from `modules/annotation/source/annotation/resource.hpp`.
By contract, `fromBytes` itself does not validate version/variant, so the authoring caller is
responsible for setting the convention.

`SourceId`, `RecognizerId`, `PageId`, and similar are distinct strong types over `ResourceId`; when
adding a source, recognizer, or page, the panel mints first and then wraps the result in the
corresponding ID. The randomness determines only the identity of a new resource and does not enter
runtime matching.
Once an ID enters the document, the canonical compiler uses it as a stable ordering/reference key.

### Source ingestion

The PNG path runs from `WorkbenchServices::m_pickPngToImport` to the `importSourcePng` in
`entry/workbench/source-ingestion.hpp`:

1. `image::loadPng` decodes the external file;
2. `assembleSource` builds the `ProjectFingerprint` from the decoded geometry and the `dpi`
   parameter, which defaults to 96. Since 2026-07-30 (`eacb05f`) that is a **parameter rather than a
   constant**: `dpi` must be the density of the *window the screenshot came from*, not of the file,
   which does not have one. `AuthoringDocument` requires every source's fingerprint to equal the
   project's, so importing at the wrong density does not degrade the result — it refuses the
   document, and a project authored from a 144-DPI window could not ingest a single file while this
   was pinned at 96;
3. the pixels are re-encoded into a canonical PNG through the pinned `image::encodeRgbaPng`;
4. the canonical bytes are run through `annotation::sha256` to obtain the `ContentHash`;
5. an `AuthoringSourceSpec` and an `AuthoringSourceAsset` with the same ID are returned, with
   `ImportedSourceProvenance` as the provenance.

Re-encoding is essential: PNGs that share the same pixels but come from different source encoders are
normalized to the project's own byte form, so later content addressing and repeated saves are
unaffected by external metadata/compression choices.

The WGC path is handled by `entry/workbench/platform/windows-capture-source.hpp`.
`captureSourceFromTargetTitle` rejects an empty substring, selects from `enumerateCandidates` the
first window whose title contains the substring and that is visible and non-iconic, resolves the
client origin/size, creates a one-shot `WgcCaptureSession`, and then takes a single `Frame` via
`captureSourceFromSession`.

`ingestSourceFromFrame` accepts only `PixelFormat::Bgra8`. It uses a full-frame `PixelRect` and
`image::cropBgra8` to remove the stride padding, converts to packed RGBA, and then runs the same
canonical encode/hash assembler.
The provenance records the frame's `TargetGeneration` and the wall-clock RFC 3339 capture time; the
monotonic timestamp is not mistakenly serialized as a calendar time.

An empty project is provisionally established by `AppState::createEmpty` with a 1280×720, 96 DPI
fingerprint. The first source replaces that placeholder when it comes through `addIngestedSource`;
every subsequent source is validated by the full `AuthoringDocument::create` and must be compatible
with the project fingerprint.

### Save, Reopen, and Publication Order

The write entry point is the `saveAndGenerateAuthoringProject` in
`entry/workbench/project-persistence.hpp`. The actual order is:

1. before creating any directory or writing final metadata, call `compileAuthoringDocument` to
   validate the document together with all source bytes and to produce the `CompiledAuthoringProject`;
2. create the project root, `assets/sources`, `assets/templates`, `generated`, and — since
   2026-07-30 (`2429578`) — an empty `tasks/`. The workbench writes no task, but the runtime
   resolves one at `<projectRoot>/tasks/<name>.luau`, so a project authored and saved here could not
   be run without the author first working out that a directory was missing and which one. It stays
   empty; present means the runtime's "no such task" is a message about the task rather than about
   the layout;
3. publish the content-addressed source PNGs referenced by the document;
4. publish the content-addressed template PNGs produced by compilation;
5. atomically replace `annotations.toml` as a single file;
6. finally, atomically replace `generated/annotations.runtime.toml` as a single file, treating it as
   the commit point of the runtime closure.

The `publishImmutableFile` in `entry/workbench/platform/windows-file-publication.hpp` verifies an
existing file byte by byte: identical means success, different means `InvalidResource`, and it never
overwrites a path that already carries the same hash. A new file is first written and flushed as a
temporary file in the same directory, then installed with
`MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`.
`replaceFileAtomically` likewise first writes and flushes a sibling temporary file, then performs a
same-volume name switch with `MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH`. Each metadata
file is atomic on its own, but the artifact set as a whole is not a transaction.

You must keep the documented non-atomic window in mind: if step 6 fails, the new `annotations.toml`
from step 5 is already visible while the old runtime manifest is still a valid runtime closure; the
new content-addressed assets may also already remain on disk. The code does not roll back. This
ordering prioritizes the guarantee that the runtime never sees a new manifest referencing an
as-yet-unpublished asset, at the cost that the authoring view and the runtime view may be
temporarily out of sync.

The read entry point `loadAuthoringProject` limits `annotations.toml` to 16 MiB, hands it to
`parseAuthoringDocument`, and then reads each `assets/sources/<hash>.png` in document order. Each PNG
is bounded by `image::k_maximumPngFileBytes`, must re-hash to equal the document record, and its
decoded width/height must equal the source fingerprint. The returned `LoadedAuthoringProject` keeps
the original PNG bytes without re-encoding, so saving directly after a load preserves byte-identical
source assets.

### Resource-Bounded Preview

The `runPreview` in `entry/workbench/preview.hpp` contains no private matcher:

1. requires the selected `SourceId` to exist;
2. calls `compileAuthoringDocument` on the current document and source assets;
3. assembles the compiler's template PNGs into `EncodedRuntimeTemplate`;
4. calls `annotation::RecognitionRuntime::create`;
5. decodes the selected PNG into a project-fingerprint-sized BGRA `Frame`;
6. calls `RecognitionRuntime::evaluatePage`;
7. if the selected recognizer is an `ActionTarget`, additionally calls `evaluateActionTarget`.

The `RecognitionPolicy` constructed by the panel provides both a comparison budget and a deadline;
the API also supports a `std::stop_token`. `PreviewResult` retains the completed `PreviewAnchorRow`,
Resolved/Unknown/Ambiguous, the resolved page ID, and the recognizer ID and `SadSearchStopReason`
inside `PreviewStop`. A stop is never collapsed into `hit=false`. The synthetic
frame/session/generation identity of the Preview frame exists only to satisfy the real recognition
API; the result does not enter the document, the history, or action delivery.

### Picking a colour key

Since 2026-07-30 (`c392161`) an element may carry a `ColourKey`, and the workbench is where one is
picked. Two functions in `entry/workbench/preview.hpp` are the whole model of that gesture:

- `sampleSourcePixel(asset, point)` is the eyedropper — picking a key is picking a pixel.
- `previewColourKeyMask(asset, templateRect, colourKey)` returns the template rectangle cropped out
  of its screen with the mask the key implies **already written into the alpha channel — the same
  bytes the compiler bakes** — plus `fullyKeptPixels` and `partiallyKeptPixels`. Without a key the
  mask is fully opaque, which is exactly what an unkeyed element compiles to.

Seeing the selection is the part that matters, and it is why the panel draws the mask back over the
crop rather than only reporting a number. A key is a guess about what is glyph and what is
background, and both failure modes — selecting almost nothing, or selecting the artwork too — are
invisible in the number and obvious in the overlay. The two counts are reported apart because they
answer different questions: how much is certainly text, and how soft the edge around it is, the
partial pixels being the antialiased rim the tolerance ramp readmits at reduced weight.

The document stores the key and the tolerance, never the mask, so an author can reopen a project and
move the tolerance while watching the selection change. See `module-annotation.md` for what the
compiler then does with it, and `module-vision-image.md` for how the matcher weights by it.

## Constraints That Must Remain True

### Fail-closed

All external input, decode, compile, capture, publish, and recognition failures go through
`Result<T>`/`Status`. Draft validation completes before history mutation; project compilation
completes before directory creation and metadata publication; load rejects missing, oversized,
hash-mismatched, and geometry-mismatched cases alike.

A recognition control stop is expressed with the variant of `PageRecognitionStop` and the completed
`PageOutcome`. Unknown/Ambiguous is an explicit outcome after full evaluation but cannot produce a
`ResolvedPage`; Cancelled, TimedOut, and ComparisonBudgetExhausted do not even masquerade as a
"miss". The Workbench, moreover, exposes no input delivery, so a Preview holds no action authority
regardless of its result.

If a content-addressed destination already holds different bytes, publication fails closed. This
collision guard protects the "a path represents its bytes" invariant and also prevents one bad save
from overwriting an old asset that the runtime may still reference.

### Determinism

Determinism extends from ingestion to publication: source PNGs are canonically re-encoded; template
crop/PNG/hash and the runtime TOML are purely generated by `compileAuthoringDocument`; document
equality uses canonical serialization; stable IDs determine the canonical ordering; and identical
documents and source bytes produce identical template bytes, hashes, and manifests.

The authoring-time `mintResourceId` is a deliberate non-deterministic boundary. It only creates a
new identity; it does not change the compile or recognition result of already-given inputs. Hit/miss
uses integer SAD and an integer basis-point threshold, and the Preview calls the production
`RecognitionRuntime` directly, with no GUI-only floating-point decision.

### Ownership and lifetime

The document, draft, history entries, source bytes, compiled artifacts, and Preview results all flow
by value or in owned containers. The pixel buffer of a `Frame` uses
`std::shared_ptr<FrameBuffer const>`, so what is shared is immutable data. `AppState`/
`RecognitionRuntime` accessors that return a reference or span are annotated `UF_LIFETIME_BOUND`, and
the caller must not treat a view as an owner.

Both `GuiShell` and `TextureCache` isolate native state behind a move-only `std::unique_ptr` PIMPL.
`GuiShellState` releases the ImGui backends/context, D3D resources, window, and registered class in
destruction order. The texture cache holds owning COM references to the D3D device and
shader-resource views; the `GpuSourceTexture` exposed to a panel is only an opaque handle valid
within the cache lifetime. As recorded in the
[2026-07-28 review follow-up](../../plans/2026-07-28-full-project-review-fixes.md), after
document edits and imports, `drawWorkbench` passes the current source list through
`WorkbenchServices::pruneTextures`; the Windows composition root routes it to
`TextureCache::pruneTo`, so deleted source IDs release their GPU views instead of accumulating for
the shell lifetime.

`GuiFrameCallback` is called synchronously by `GuiShell::run` and is not stored; the
`WorkbenchServices` callbacks are borrowed only within a single draw. The reference captures in
`main.cpp` are safe precisely because of this synchronous protocol: `state`, `services`, `ui`, and
`shell` all outlive the entire `run` call.

### Platform/SAFETY boundary

Win32 handle bit restoration, `GWLP_USERDATA` pointer conversion, COM out parameters, D3D texture
handle conversion, Win32 path pointers, and file I/O buffers are all confined to
`entry/workbench/platform/*.cpp`. Every raw/native operation carries a nearby `// SAFETY:`, and the
headers expose as much as possible only values, RAII objects, `Result`, and opaque integer handles;
ImGui/D3D11/Win32 types do not enter `AppState` or the authoring backend.

strict-background input does not apply in this subsystem: the Workbench is an interactive, ordinary
GUI, but it only performs WGC capture against the target and does not send mouse/keyboard input. The
product's background-delivery discipline is enforced by the runtime controller action sink; adding
an input API to `WorkbenchServices` would cross the existing responsibility boundary and also violate
the "runtime input from the workbench" that `docs/plans/2026-07-22-annotation-design.md` explicitly
excludes.

## Dependencies

The inbound edge runs from the user/OS to the Workbench:

- the command line provides the project root or the `--smoke` frame budget;
- ImGui widgets produce draft edits, selection, canvas gestures, and action requests;
- the file dialog provides a PNG path;
- controller discovery/WGC provides a complete BGRA `Frame`;
- the `annotations.toml` and source PNGs on disk provide the reopen input.

The inbound type conversions are concentrated at the boundary: a PNG path/`Frame` becomes an
`IngestedSource`, a widget buffer becomes an `AuthoringDraft`, and disk bytes become a validated
`AuthoringDocument`/`AuthoringSourceAsset`. Once inside the app/backend, nothing carries an HWND, a
D3D pointer, or ImGui widget state any longer.

The outbound edge runs from the Workbench to the reusable modules and the filesystem:

- to `annotation`, it passes the document, source assets, and recognition policy, and takes back the
  validated document, the compiled project, and evidence;
- to `image`, it passes encoded/decoded pixel bytes and takes back a canonical PNG or a layout
  conversion;
- to `controller`, it passes the selected target handle/geometry and takes back a captured frame;
- to the filesystem, it publishes source PNGs, template PNGs, the authoring TOML, and the runtime
  TOML;
- to the panel, it returns status strings, texture handles, and transient Preview rows.

`umbra-workbench` does not link `engine`. The recognition core actually shared with the runtime is
`annotation::RecognitionRuntime`; the runtime consumes only the
`generated/annotations.runtime.toml` and template assets that the Workbench publishes last, and
there is no in-process interface between the two. That build edge existed for a while with no
source referencing it and was removed on 2026-07-26 -- do not restore it to bring an engine session,
lease, or action port into the app state, which would cross the existing responsibility boundary.

The stable identifier across boundaries is the strong `ResourceId` wrapper, the integrity credential
across disk is `ContentHash`, the compatibility credential across capture/authoring is
`ProjectFingerprint`, and the information across recognition/UI is the flat
`PreviewAnchorRow`/`PreviewStop`. These narrow values keep panels from depending on the private
evidence layout of `RecognitionRuntime` or on native platform objects.

## Tests

On Windows, `tests/CMakeLists.txt` combines seven synthetic files into `test-workbench` and links
`umbraflow_workbench_support` directly, thereby bypassing ImGui and the real desktop:

- `tests/workbench/test-authoring-edit.cpp` locks down the full draft round trip, validated apply, an
  invalid draft leaving history unchanged, an identical edit, redo branch/replay, and the 100-entry
  undo boundary.
- `tests/workbench/test-workbench-app.cpp` locks down UUID v4/variant, empty state, dirty/history,
  selection/view, orphan cache filtering, redo restoring a source, compiling only the new source
  after a branch, and Preview invalidation after an edit.
- `tests/workbench/test-canvas-math.cpp` locks down source/screen inverse, anchor-preserving zoom,
  zoom-scaled pan, grip priority/hit test, and the source bounds and one-pixel minimum for
  resize/move.
- `tests/workbench/test-source-ingestion.cpp` locks down PNG canonical encode/hash, non-PNG
  rejection, BGRA→RGBA WGC provenance, stride-padding removal, and non-BGRA rejection.
- `tests/workbench/test-project-persistence.cpp` locks down the full save/reopen, single-file atomic
  replacement, pre-publish validation, source-asset reorder mapping, immutable collision rejection,
  the absence of temporary residue, the load round trip, and hash-mismatch and missing-source
  rejection. It does not mis-assert the whole artifact set as a transaction.
- `tests/workbench/test-preview.cpp` locks down resolved page/anchor evidence, selected action
  evidence, the zero-budget stop reason, and absent-source rejection, proving that the UI wrapper
  preserves the runtime outcome.
- `tests/workbench/test-model-check-job.cpp` locks down the background job that carries the whole
  model check: delivery exactly once, an error surfacing rather than being swallowed, running until
  the work returns, a second start leaving an in-flight run alone, a discarded run never reaching
  the status line, a discarded run not blocking the next one, and the destructor cancelling and
  joining its worker. It drives `startWith` with fake work, so none of it pays for a pixel sweep.

`tests/workbench/test-real-regression.cpp` is a separate, local-only `REAL` suite. CMake registers it
only when `tests/assets/real-regression` exists; it walks the uncommitted real projects, calls
`loadAuthoringProject` and the annotation regression runner, and replays against the recorded
expectations. CI relies only on synthetic fixtures, and real screenshots do not enter the repository.

The GUI shell, the file dialog, and title-based WGC capture have no deterministic unit test;
`--smoke` covers only the shell startup/pump loop for a limited number of frames, and real target
capture and visual interaction still require verification on real Windows hardware. When modifying
platform lifetime or the D3D/ImGui wiring, a green `test-workbench` cannot substitute for
smoke/manual evidence.

## Future Extensions

`docs/plans/2026-07-21-product-form-and-roadmap.md` is the authority on product cadence: A2 extends
required/forbidden across multiple anchors/pages, Unknown/Ambiguous, and sample Preview/Test; A3
extends batch, sample management, and the static regression UX. These features should attach
respectively to the existing `AuthoringDraft`/`applyEdit`, `runPreview`/`PreviewResult`, and the
annotation regression runner, and should not establish a bypass document or matcher inside the
panels.

`docs/plans/2026-07-22-annotation-design.md` is the authority on the S0 schema, recognition,
artifact, and workbench contract. When adding an annotation type, a schema field, resolution
adaptation, OCR, or recognition-policy semantics, first establish the canonical behavior in the
successor authority of that contract and in `modules/annotation`/`vision`, and only then wire the
editing control to the Workbench.

The natural seam for a future extraction of `modules/authoring` is the ImGui-independent public
surface of the current `umbraflow_workbench_support`: `AuthoringEditHistory`, ingestion, the
compile-facing persistence model, the Preview adapter, and the document/cache rules of `AppState`
together with their synthetic tests. The existing code already separates the panels behind a
value/`Result` API, so a consumer does not need to know Dear ImGui.

But the whole support target cannot be moved as is: `project-persistence.cpp` calls the Windows
file-publication adapter directly, WGC ingestion depends directly on `controller`, and `AppState`
holds both project semantics and GUI selection/view state. A real extraction should keep the pure
authoring policy, let publication/capture continue to be injected as ports from the entry, and leave
GUI-only state in the entry; otherwise the new module is merely a Windows composition root in a
different directory. The current `docs/plans/` does not designate a separate phase for
`modules/authoring`, so this paragraph describes an existing, verifiable seam and does not claim an
approved migration schedule.

Hardening the publication transaction is also a separate seam.
`docs/plans/2026-07-23-engine-architecture.md` explicitly excludes the Workbench publication
rollback-window fix from the current phase. In the future, a generation directory/journal or an
equivalent commit protocol could be added between `saveAndGenerateAuthoringProject` and
`windows-file-publication`; until then, the existing "assets first, authoring second, runtime
manifest last" ordering must be maintained, and the non-atomic window must be acknowledged in error
handling and tests.

Platform extensions should continue to go through `WorkbenchServices` and the `platform/` RAII
wrappers: a new picker, capture selector, or renderer only replaces the service/opaque-texture
implementation. Recognition extensions, by contrast, enter through `compileAuthoringDocument`,
`RecognitionRuntime`, and `PreviewResult`. Distinguishing OS capability from product semantics
through these two kinds of seam is the key to keeping the Workbench navigable, testable, and
extractable in the future.
