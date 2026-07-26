# P0-A Visual Annotation System & Data Model — S0 Locked Design

> Status: **S0 LOCKED — developer-approved 2026-07-23**.
>
> This document is the authority for the shared S0 annotation contract and the
> P0-A workbench. It incorporates the 2026-07-22 developer review and resolves
> all nine former open questions. A1 and B1 may build against this contract.
>
> Current authority chain: this document specializes the product direction in
> [`2026-07-21-product-form-and-roadmap.md`](2026-07-21-product-form-and-roadmap.md)
> and the language-independent task semantics retained by
> [`2026-07-21-lua-task-model-grill-decisions.md`](2026-07-21-lua-task-model-grill-decisions.md).
> The old Rust-era `DESIGN.md` is historical input only. Every retained rule is
> restated here; the old document is not normative.
>
> Load-bearing constraints: determinism, complete trace evidence, strict
> background operation, zero game-specific branches in core, assets separate
> from code, and read-only opaque Luau handles. Unknown, Ambiguous, stopped
> recognition, stale observations, and incompatible geometry must produce
> exactly zero Controller input deliveries.

## 0. Grounding facts from the ported code

| Fact | Location | Locked consequence |
|---|---|---|
| `Desktop→Client→Frame→Normalized` spaces already exist; `PixelRect` is validated integer storage. | `modules/domain/source/domain/space.hpp` | Annotation geometry is integer `FrameSpace` at one project `base_resolution`; no new P0 coordinate space is introduced. |
| `CoordinateTransform` maps live Client and Frame geometry. | `space.hpp` | It remains the live Client↔Frame transform. P1 resolution adaptation requires a separate explicit Base→Live viewport transform; it is not represented by changing this existing value. |
| SAD returns a `uint64` distance: lower is better and zero is perfect. | `modules/vision/source/vision/sad.{hpp,cpp}` | The authored threshold is fixed-point and compiles to an integer inclusive `maxSad` bound. Floating point never decides hit/miss. |
| The bounded SAD overload reports `Cancelled`, `TimedOut`, and `ComparisonBudgetExhausted`. | `sad.hpp` | Runtime and Preview always use the bounded overload and preserve every stop reason as a control failure. |
| Grayscale conversion is deterministic integer math. | `sad.cpp` `bgra8ToGray8` | Runtime and Preview call this exact function; the workbench has no private matcher. |
| The matcher consumes an integer search `PixelRect`. | `sad.cpp` | `template_rect` and `search_roi` are separate integer rectangles. The former creates the template; the latter bounds live-frame search. |
| `Detection` and `ObservationLease` carry session, target generation, and frame identity; default action age is 750 ms. | `modules/domain/source/domain/detection.hpp` | A live Detection remains necessary but is not sufficient for a page-dependent action; the action also requires same-frame `ResolvedPage` evidence. |
| WGC capture returns the complete current client frame and a live transform. | `modules/controller/source/controller/capture.hpp` | The full WGC frame is runtime input. Cropped templates are the static runtime assets. |

## 1. Artifact and schema model

### 1.1 Artifact ownership and packaging

P0 uses a two-layer document model: a complete GUI authoring document and a
deterministically generated runtime manifest.

| Artifact | Owner and purpose | Production runtime package |
|---|---|---|
| `project.toml` | Hand-authored project, target, capability, and task configuration. | Included. |
| `annotations.toml` | GUI-owned authoring source of truth: source images, stable IDs, rectangles, page membership, click anchors, and regression references. | Excluded. |
| `assets/sources/<hash>.png` | Exact full screenshots required to reopen and edit the authoring project. | Excluded. |
| `assets/templates/<hash>.png` | Lossless templates cropped from `template_rect`. | Included when referenced. |
| `generated/annotations.runtime.toml` | Pure generated runtime closure containing recognizers and pages only. | Included. |
| Regression screenshots | Selected full-frame golden inputs for Preview, CI, and Fake Controller sequences. | Development/test bundle only. |

Exploratory screenshots that are not retained by the authoring document or a
regression set are local scratch data and are never packaged.

```toml
# project.toml — hand-authored
[project]
id = "personal.chaos-dreamscape"

[targets.windows]
baseline_client_size = [1920, 1080]
baseline_dpi = [96, 96]

[annotations]
source = "annotations.toml"
runtime = "generated/annotations.runtime.toml"
```

`baseline_client_size` and `baseline_dpi` form one project-wide P0
compatibility fingerprint. No recognizer may override either value. The Runtime
reads only `annotations.runtime`; `annotations.source` is workbench-only and may
be absent from a runtime-only deployment.

### 1.2 Authoring document

P0 supports the exact schema identifier `umbraflow-authoring/v1`. Any other
identifier is rejected; minor-version best-effort loading is deferred.

```toml
schema = "umbraflow-authoring/v1"
project_id = "personal.chaos-dreamscape"
base_resolution = [1920, 1080]
base_dpi = [96, 96]

[[source]]
id = "2f49b58e-16d0-46a5-89ff-14db2bed31b9"
path = "assets/sources/4c91...a2.png"
content_hash = "sha256:4c91...a2"
client_size = [1920, 1080]
dpi = [96, 96]
capture_backend = "wgc"
target_generation = 7
captured_at = "2026-07-23T09:15:00+09:00"

[[annotation]]
id = "bf61fe8d-31f7-4c2f-8b6e-36c2c08054dc"
name = "home_marker"
type = "page_anchor"
source_id = "2f49b58e-16d0-46a5-89ff-14db2bed31b9"
recognizer_kind = "gray_template"
template_rect = [1240, 48, 72, 36]
search_roi = [1180, 24, 220, 96]
min_similarity_bp = 9000

[[annotation]]
id = "46fd32d7-095b-427f-9892-bd86ee2ab073"
name = "daily_button"
type = "action_target"
source_id = "2f49b58e-16d0-46a5-89ff-14db2bed31b9"
recognizer_kind = "gray_template"
template_rect = [1480, 760, 160, 64]
search_roi = [1400, 700, 300, 160]
min_similarity_bp = 9000
default_click = [80, 32] # integer template-local offset
page_ids = ["c1ec5b68-3c29-448d-ac65-ad3798684e3c"]

[[page]]
id = "c1ec5b68-3c29-448d-ac65-ad3798684e3c"
name = "home"
required = ["bf61fe8d-31f7-4c2f-8b6e-36c2c08054dc"]
forbidden = []

[[regression]]
id = "f27cf10d-ce50-4216-96e5-1bf579bf6251"
source_id = "2f49b58e-16d0-46a5-89ff-14db2bed31b9"
classification = "positive"
expected_outcome = "resolved"
expected_page_id = "c1ec5b68-3c29-448d-ac65-ad3798684e3c"
```

All relationships use stable UUIDs. Names are unique human-readable labels and
Luau keys; renaming changes the name without changing identity. The workbench
updates every generated name reference atomically. P0 does not rewrite Luau
source: its pre-VM AST validation reports every stale literal after a rename,
and the author updates those literals. Automatic Luau reference rewriting is a
P1 authoring convenience.

The authoring schema persists every source record, annotation type, page role,
rectangle, default click, and regression reference needed for an exact
save/reload round trip. Imported PNGs record imported provenance rather than
inventing WGC-only fields.

`annotations.toml` is GUI-owned. The workbench emits one canonical byte form
with fixed table/field/UUID order, UTF-8 without BOM, LF endings, and one final
newline; reopening rejects schema drift and non-canonical edits. The annotated
TOML snippets in this design are explanatory rather than literal writer output.
A WGC source uses `capture_backend = "wgc"` and requires
`target_generation` plus canonical RFC 3339 `captured_at`. An imported PNG uses
`capture_backend = "imported"` and omits those WGC-only fields.

> **Amendment (2026-07-26).** By user decision, and per
> [`2026-07-26-page-centric-authoring.md`](2026-07-26-page-centric-authoring.md)
> phase 2, three clauses of this section are overridden for the **authoring**
> document only: the "exact schema identifier `umbraflow-authoring/v1`" clause,
> the "minor-version best-effort loading is deferred" clause, and the "reopening
> rejects schema drift" clause. The GUI now writes `umbraflow-authoring/v2`
> (page membership moved off the recognizer onto page-side placements), and a
> read-only migration loader accepts a `umbraflow-authoring/v1` file once,
> upgrading it to v2 on save. That migration is precisely the deferred
> best-effort capability, now un-deferred. Every other S0 clause remains in
> force — above all the runtime manifest schema `umbraflow-annotations/v1`
> (§1.3), which does not change.

### 1.3 Generated runtime manifest

The workbench compiles the authoring document into exact schema
`umbraflow-annotations/v1`. The Runtime reads only this manifest and cropped
templates; it never needs the full authoring screenshots.

```toml
schema = "umbraflow-annotations/v1"
project_id = "personal.chaos-dreamscape"
base_resolution = [1920, 1080]
base_dpi = [96, 96]

[[recognizer]]
id = "46fd32d7-095b-427f-9892-bd86ee2ab073"
name = "daily_button"
annotation_type = "action_target"
kind = "gray_template"
template = "assets/templates/83d1...e4.png"
template_hash = "sha256:83d1...e4"
source_hash = "sha256:4c91...a2"
template_rect = [1480, 760, 160, 64] # provenance only
search_roi = [1400, 700, 300, 160]
min_similarity_bp = 9000
default_click = [80, 32]
allowed_page_ids = ["c1ec5b68-3c29-448d-ac65-ad3798684e3c"]

[[recognizer]]
id = "bf61fe8d-31f7-4c2f-8b6e-36c2c08054dc"
name = "home_marker"
annotation_type = "page_anchor"
kind = "gray_template"
template = "assets/templates/7f3a...c9.png"
template_hash = "sha256:7f3a...c9"
source_hash = "sha256:4c91...a2"
template_rect = [1240, 48, 72, 36] # provenance only
search_roi = [1180, 24, 220, 96]
min_similarity_bp = 9000

[[page]]
id = "c1ec5b68-3c29-448d-ac65-ad3798684e3c"
name = "home"
required = ["bf61fe8d-31f7-4c2f-8b6e-36c2c08054dc"]
forbidden = []
```

Generation is a pure function of the authoring document plus referenced source
bytes. Identical inputs produce byte-identical templates and manifest output.
The compiler writes all generated artifacts atomically only after complete
validation succeeds.

The P0 in-memory compiler decodes one full source at a time and caps the sum of
unique generated template PNG bytes at 512 MiB. Authoring/runtime documents are
limited to 16 MiB and 4096 records per table kind. Quota failure is
`InvalidResource` and publishes no partial generation.

The generated TOML has one canonical byte representation: fixed field order,
UUID order, UTF-8 without BOM, LF line endings, and one trailing newline. The
P0 Runtime accepts exactly compiler output and rejects comments, unknown fields,
alternate ordering, non-canonical numeric/string spellings, and template paths
that do not match `template_hash`. Generated files are never hand-edited.

### 1.4 Deterministic threshold

The workbench displays a familiar percentage but persists integer basis points:
`min_similarity_bp ∈ [0, 10000]`; `9000` means 90.00%.

For a template with `templatePixels = width × height`, load computes with
checked integer arithmetic:

```text
maxSad = floor((10000 - minSimilarityBp) × 255 × templatePixels / 10000)
hit    = sadScore <= maxSad
```

Overflow or an invalid range is `InvalidResource`. The inclusive equality is
part of the contract. A display-only confidence may be computed as
`1 - sadScore / (255 × templatePixels)`, but it never participates in a
decision or ordering rule.

### 1.5 Annotation types

| Type | Purpose | Runtime form |
|---|---|---|
| `page_anchor` | Positive or negative evidence for one or more page signatures. | Read-only recognizer referenced by a page. |
| `action_target` | A target that may be acted on after live detection. | Recognizer plus optional template-local integer click offset; absence means the deterministic rectangle center. The live Detection and ResolvedPage still authorize the action. |
| `info_region` | An icon or state region read by the task. | P0 gray-template recognizer. OCR is admitted only under the trigger in §7. |

## 2. Coordinate and compatibility contract

The source of truth is integer **AnnotationSpace = FrameSpace at the project
`base_resolution`**. `NormalizedSpace` is derived for display only and is never
persisted as recognition geometry.

```text
AnnotationSpace (base px)
    -- P0 identity / P1 BaseToLiveTransform --> live FrameSpace
    -- existing CoordinateTransform --------> ClientSpace --> DesktopSpace
```

P0 has no scaling or resampling. Before every recognition cycle, capture must
confirm that live frame size, client size, and integer DPI exactly equal the
project fingerprint. Immediately before Controller delivery, the action path
revalidates the same fingerprint together with session, target generation,
frame ID, and lease age.

A size, DPI, target identity, or transform mismatch must either advance target
generation or fail with `TargetCompatibilityUnverified`; in both cases all old
page evidence, Detections, and leases become unusable before any input.

P1 may introduce an explicit `BaseToLiveTransform { uniformScale, offset,
viewport }` and deterministic template resampling. That value is separate from
the existing live Client↔Frame `CoordinateTransform`, and its complete value is
recorded in trace. No P1 behavior is silently present in P0.

## 3. Recognition and page resolution

### 3.1 One bounded RecognitionPolicy

Runtime and Preview construct the same bounded `RecognitionPolicy`, including
the maximum pixel-comparison budget, cancellation source, and deadline. They
both call the bounded `matchTemplateSad` overload.

Every anchor evaluation has one of two classes of outcome:

- completed: best match or no match, followed by the integer threshold test;
- stopped: `Cancelled`, `TimedOut`, or `ComparisonBudgetExhausted`.

Any stopped anchor terminates the complete page evaluation. It is never folded
into `hit=false`, including for forbidden anchors. Page resolution therefore
returns `Result<PageOutcome>` (or an equivalent explicit control variant), and
trace records the policy, completed comparisons, stop reason, and anchor at
which evaluation stopped.

### 3.2 Page signature

```toml
[[page]]
id = "c1ec5b68-3c29-448d-ac65-ad3798684e3c"
name = "home"
required = ["bf61fe8d-31f7-4c2f-8b6e-36c2c08054dc"]
forbidden = ["8a467172-a8dc-4c02-9a73-24abbdc35df4"]
```

A page is a candidate exactly when all required anchors hit and no forbidden
anchor hits. The generator sorts recognizers, pages, and page membership arrays
by stable UUID; that byte-stable manifest order is also evaluation order. Every
page is evaluated before success may be returned, so an earlier candidate can
never hide a later ambiguity. For candidate set `C`:

Every page signature contains at least one required or forbidden anchor. An
empty signature is `InvalidResource`; P0 does not interpret it as an implicit
default or fallback page.

| `|C|` | Completed `PageOutcome` |
|---|---|
| 1 | `ResolvedPage(pageId, frame identity, evidence)` |
| 0 | `Unknown(evidence)` |
| 2 or more | `Ambiguous(pageIds, evidence)` |

P0 has no priority, threshold override, or heuristic tie-break. Ambiguity can
only be removed by changing page evidence. Exact duplicate signatures are a
load-time error; all other overlap is exercised through regression frames and
remains fail-closed at runtime.

### 3.3 Evidence and action authorization

Every completed anchor records its recognizer ID, hit, integer SAD score,
integer `maxSad`, matched rectangle when present, and display confidence. Every
PageOutcome records session ID, target generation, frame ID, candidate set, and
per-page required/forbidden evidence.

`ResolvedPage` is an opaque, host-created, same-frame capability. P0 coordinate
actions require all of the following:

1. a `ResolvedPage` from the frame;
2. a live `Detection` from that same frame;
3. the frame's unexpired `ObservationLease`;
4. a still-compatible target fingerprint at Controller delivery.

The host checks that all identities agree, that the Detection came from an
`action_target`, and that the recognizer authorizes the resolved page through
its non-empty `allowed_page_ids`. The opaque binding retains recognizer identity
alongside the domain Detection so a label collision cannot authorize an action.
Unknown and Ambiguous cannot create a
`ResolvedPage`, so they are unrepresentable as successful action authorization.
After one coordinate action, FrameId advances and all evidence from the old
frame becomes stale.

### 3.4 Load-time validation

Before creating a Luau VM, loading must prove:

- the exact supported schemas and project fingerprint agree;
- all UUIDs and names are unique, and names are valid direct Luau member keys;
- every stable-ID reference closes, every page has at least one required or
  forbidden anchor, and `required ∩ forbidden` is empty;
- page signatures reference only `page_anchor` entries, while every
  `action_target`/`info_region` page membership resolves;
- every `action_target` has at least one allowed page, and any explicit click
  offset lies inside its template rectangle;
- every source/template exists and its content hash matches;
- every source used to generate an annotation decodes at the project base
  resolution; WGC sources also match base DPI, while imported sources must carry
  an explicit compatible fingerprint;
- `template_rect` and `search_roi` are non-empty and within base resolution;
- template dimensions fit inside `search_roi`;
- every basis-point threshold and template-local click offset is in range;
- every recognizer kind is exactly `gray_template` in P0;
- no two pages have the exact same required and forbidden sets.

Any failure is `InvalidResource`. P0 has no best-effort schema loading or static
ambiguity heuristic beyond the provable exact-duplicate check.

## 4. Luau resource and action surface

The host creates recursively read-only tables from the validated runtime
manifest. P0 exposes one canonical spelling only:

```lua
local frame = bot:capture()
local outcome = frame:resolve_page()
local page = outcome:resolved() -- ResolvedPage | nil; Unknown/Ambiguous already traced

if page ~= nil and page:is(bot.pages.home) then
    local target = frame:find(bot.recognizers.daily_button)
    if target ~= nil then
        bot:click(page, target)
    end
end
```

- `bot.recognizers.<name>` contains opaque recognizer handles.
- `bot.pages.<name>` contains opaque page handles.
- There is no `bot.templates` alias, path constructor, string-path lookup, or
  script-visible ROI/pixel data.
- `bot:click` requires `ResolvedPage` and `Detection`; a default click is only a
  host-applied template-local offset on the live Detection.

P0 script validation is deliberately restricted and decidable. Every resource
expression must be a direct literal member access rooted at the canonical
namespace. Namespace aliases, handle aliases, computed indexing, and dynamic
traversal are rejected. The Luau AST validator enumerates every accepted
reference, resolves it against the manifest, and rejects the script before VM
creation if any reference is missing. Runtime nil checks remain defense in
depth, not the primary closure mechanism.

## 5. Annotation workbench

The P0-A workbench uses Dear ImGui docking on D3D11. WGC capture is reused
verbatim, and Preview links the same grayscale conversion and bounded SAD
matcher as Runtime.

The non-deferrable author loop is:

1. Select a target and capture its current full WGC frame, or import lossless
   PNGs. Preserve exact source pixels and provenance; never draw annotations
   into the source image.
2. Browse sources and zoom/pan the canvas. Create, move, resize, copy, and delete
   annotations with undo/redo.
3. Edit `template_rect` and `search_roi` as visibly distinct rectangles. The UI
   must make their containment and size relationship obvious.
4. Edit stable name, annotation type, page membership, required/forbidden role,
   integer-backed similarity percentage, and optional action click offset.
5. Save the complete authoring document, then generate templates and runtime
   manifest atomically. Authors never hand-edit generated files.
6. Preview one source or all regression sources through the shared bounded
   RecognitionPolicy. Display matches, SAD boundaries, expected/actual page,
   Unknown/Ambiguous evidence, and stop reasons.
7. Add a result to the positive, negative, or confusable regression set without
   silently changing its expected outcome.

Preview results are transient and never enter the authoring document or
undo/redo history. Manifest generation is a pure function. Undo/redo behavior
is required, but S0 does not prescribe a command-class implementation.

## 6. Asset generation and tests

For each gray-template annotation:

1. Crop only `template_rect` from the exact source BGRA buffer.
2. Encode a lossless PNG with the pinned deterministic encoder configuration.
3. Hash the generated PNG bytes with SHA-256 and address the template by hash.
4. Persist `search_roi` independently in the runtime manifest.
5. Retain source hash and crop geometry as provenance; derive grayscale at load
   with the shared `bgra8ToGray8` implementation.

The complete source screenshot is never used as the static matcher template.
At runtime, WGC supplies a new complete live frame; the cropped template is
searched only inside its independent `search_roi`.

Selected full screenshots form content-addressed golden regression inputs with
an expected PageOutcome. Fake Controller sequences reuse those frames to drive
the complete observe/resolve/act/wait loop offline. A live run may copy selected
full WGC frames into its trace resource snapshot; those are run evidence, not
static project recognition assets.

Minimum verification gates are:

1. **Zero-input fail-closed:** Unknown, Ambiguous, every matcher stop reason,
   stale frame, cross-generation evidence, and geometry/DPI mismatch produce
   structured trace and exactly zero Controller deliveries.
2. **Recognition boundaries:** cover `sadScore == maxSad`, both adjacent values,
   threshold endpoints, all stop reasons, and field-for-field Preview/Runtime
   evidence equality.
3. **Authoring round trip:** capture/import → edit both rectangles → undo/redo →
   save → reload preserves exact source pixels, stable IDs, geometry, template
   bytes/hash, page links, and generated manifest.
4. **Real-machine compatibility:** cover resize, recreation, minimization/stall,
   DPI mismatch, and movement between displays; invalidation precedes input.

## 7. Explicit P0 scope

P0 implements only bounded deterministic grayscale-template recognition. Color,
HSV, composite recognizers, OCR, parameterized ROI, priority, per-page threshold
override, per-recognizer resolution, schema minor best-effort loading, static
`--strict` ambiguity heuristics, and duplicate handle aliases are deferred.

OCR may enter P0 only if a concrete 卡厄斯梦境 daily requires semantic dynamic
text or numbers and the behavior cannot be expressed with templates or state
anchors. That exception requires a separate deterministic-kernel decision.

P0 also excludes resolution adaptation, task-graph editing, script recording,
asset marketplace, multi-user/project management, HTML reports, packaging and
signing, runtime input from the workbench, and the P2 no-activate runtime overlay.
The complete author loop in §5 is not deferrable as “UI later.”

## 8. Developer-approved decisions — 2026-07-23

| Former question | Locked decision |
|---|---|
| OQ-1 threshold unit | UI percentage, persisted integer basis points, integer-inclusive SAD boundary. |
| OQ-2 page outcome/error model | `Result<PageOutcome>`; control stops are errors, Unknown/Ambiguous are completed values, and only `ResolvedPage` authorizes action. |
| OQ-3 static ambiguity | Exact duplicate signatures are errors; no P0 heuristic or `--strict` mode. |
| OQ-4 info/OCR | Gray template by default; OCR only under the concrete blocking trigger in §7. |
| OQ-5 source screenshots | Full sources stay in authoring/test storage; production runtime receives only generated manifest and cropped templates. |
| OQ-6 base resolution | One project-wide resolution and exact integer DPI fingerprint; no recognizer override. |
| OQ-7 handle namespace | Canonical `bot.recognizers` and `bot.pages` only, with direct literal references. |
| OQ-8 composite | Deferred beyond P0. |
| OQ-9 undo/Preview | Preview is transient; generation is pure; undo/redo behavior is required without prescribing one implementation architecture. |

## 9. Developer review closure

The 2026-07-22 review disposition was “revision required.” The developer
approved the resolutions on 2026-07-23, and this revision closes every finding:

| Finding | Resolution in this document |
|---|---|
| DR-1 page evidence did not authorize actions | `ResolvedPage` is mandatory action capability (§3.3, §4). |
| DR-2 matcher stops could become misses | One bounded policy and explicit control failure (§3.1). |
| DR-3 crop and search ROI were conflated | Separate `template_rect` and `search_roi` (§1.2–§1.3, §5–§6). |
| DR-4 schema could not round-trip | Complete authoring document plus pure runtime compilation (§1). |
| DR-5 speculative recognizer modes and float threshold | Gray SAD only and integer basis points (§1.4, §7). |
| DR-6 compatibility was inconsistent and checked once | One project fingerprint, continuously revalidated (§2). |
| DR-7 script resource closure was best-effort | Decidable AST subset and complete pre-VM resolution (§4). |
| DR-8 authority and status conflicted | Current authority restated; all decisions closed; S0 locked (header, §8). |

### References

- ok-script framework and annotation/debug-overlay UX shape: https://github.com/ok-oldking/ok-script
- COCO annotation tooling shape: https://github.com/jsbroks/coco-annotator
- Multi-scale template matching caveats for P1: https://pyimagesearch.com/2015/01/26/multi-scale-template-matching-using-python-opencv/
- Letterbox/viewport background for P1: https://gamedev.net/tutorials/_/technical/apis-and-tools/stretching-your-game-to-fit-the-screen-without-letterboxing-sdl2-r3547/
- Windows DPI logical/physical pitfalls: https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-screenscaling
- Opaque locator discipline: https://www.browserstack.com/guide/stale-element-reference-exception-selenium and https://playwright.dev/docs/actionability
