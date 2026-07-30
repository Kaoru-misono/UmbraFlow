# annotation Module Architecture Knowledge

> **DIRTY (2026-07-26)**: This document predates authoring schema v2
> (Element+placement replacing per-page copies, the v1 read-path sunset,
> per-placement runtime recognizer expansion in the compiler, and the
> deriveModel permanent bridge). Trust the code and
> `docs/plans/2026-07-26-page-centric-authoring.md` until resynced.
>
> The colour key added on 2026-07-30 (`c392161`) **is** written up below, in
> "Colour keys and the template mask" and in the runtime section, because it
> changes what a compiled template's bytes mean.

This document describes the S0 contract already implemented by `modules/annotation`. The complete
design is in `docs/plans/2026-07-22-annotation-design.md`; this guide focuses on code entry points,
processing flow, and constraints that must remain true.

## Module Responsibilities

`annotation` converts authored annotations into recognition evidence that runtime can authorize. It
owns six categories of responsibility:

1. Define stable resource identities, project fingerprints, recognizers, and page signatures, and
   close their reference relationships at object construction time.
2. Define the precise S0 schema of the GUI authoring document and the runtime manifest, along with
   the single canonical TOML byte form.
3. Deterministically crop, encode, and hash templates from full source screenshots, and compile the
   authoring document into a content-addressed runtime closure.
4. Evaluate page anchors and action targets with a single bounded grayscale SAD recognition path,
   fully preserving hit evidence, work volume, and stop reasons.
5. Converge the page candidate set into `ResolvedPage`, `UnknownPage`, or `AmbiguousPages`, without
   priority or heuristic disambiguation.
6. Before coordinate action delivery, validate page capability, action detection, observation lease,
   and live compatibility, giving engine a fail-closed authorization result.

The module publicly depends on `core`, `domain`, and `vision`, and privately depends on `image`; see
`modules/annotation/manifest.txt`. This dependency direction embodies the boundary:

- `domain` provides `Frame`, `Detection`, `ObservationLease`, coordinate spaces, and identity
  values; `annotation` composes them but does not redefine capture or lease semantics.
- `vision` owns `GrayImage`, `bgra8ToGray8`, and the bounded `matchTemplateSad`; `annotation` decides
  how to order work, set thresholds, and interpret results, without building its own matcher.
- `image` is responsible only, inside the implementation, for PNG decoding, pixel-layout conversion,
  cropping, and encoding, so consumers do not need to depend directly on the image codec.

It deliberately does not own the following capabilities:

- It does not capture windows, discover targets, deliver clicks, or implement Windows `PostMessage`.
  These belong to the controller adapter and engine composition respectively.
- It is not responsible for atomic filesystem publication. The workbench publication order is in
  `entry/workbench/project-persistence.cpp`, and engine's disk-loading boundary is in
  `modules/engine/source/engine/runtime-loader.cpp`.
- It is not responsible for the Luau VM, script AST, or opaque handle exposure; here it only
  validates that `ResourceName` is a directly accessible ASCII Luau member key and provides strongly
  typed resources that the host can wrap.
- It does not make floating-point threshold decisions, color/HSV/OCR/composite recognizers,
  resolution scaling, page priority, or best-effort schema compatibility.
- It does not guarantee that "a click will always succeed." It only proves that the annotation-side
  authorization conditions hold; engine must still re-verify the target instance before the sink
  call, and controller must still validate the lease at the actual delivery layer.

Thus `strict-background` here manifests as "cannot authorize a suspicious coordinate action," rather
than an input backend. The real background delivery lies outside the module; `annotation`'s
responsibility is to ensure that `Unknown`, `Ambiguous`, recognition stop, expired evidence, or
incompatible geometry can never cross the authorization boundary.

## Data Model and Processing Flow

### Identity, Geometry, and Catalog

The shared resource vocabulary begins at
`modules/annotation/source/annotation/resource.hpp`; catalog definitions build on it in
`modules/annotation/source/annotation/catalog.hpp`:

- `ResourceId` holds a 16-byte UUID; `parse` accepts the fixed 36-character UUID shape, and
  `toString` emits the lowercase canonical form.
- `ElementId`, `PageId`, `SourceId`, and `RegressionId` are distinct `StrongValue`s built on
  `ResourceId`, preventing cross-category resource mix-ups.
- `ProjectId` requires non-empty valid UTF-8; `ResourceName` requires a non-empty ASCII identifier
  that is not a Luau reserved word.
- `ProjectFingerprint` is a non-zero `width`, `height`, `dpiX`, and `dpiY`. S0 fixes the
  AnnotationSpace as the integer `FrameSpace` at this base resolution.
- `AnnotationType` currently has `PageAnchor`, `ActionTarget`, and `InfoRegion`. The S0 schema's
  recognizer kind is still only `gray_template`.

`RecognizerDefinition::create` is the validation entry point for a single recognizer. Given a
`RecognizerSpec`, it validates that `templateRect` and `searchRoi` both lie within the project
bounds, that the template size fits inside the ROI, that the threshold is computable, that a click
belongs only to an `ActionTarget` and falls inside the template, that a `PageAnchor` carries no page
membership, and that an `ActionTarget` authorizes at least one page. It then sorts `allowedPageIds`
by `PageId` and rejects duplicates.

`PageSignature::create` accepts a `PageSpec`, requires that at least one of `required` and
`forbidden` be non-empty, sorts each by `ElementId`, and rejects duplicates and intersections. The
"non-empty" requirement matters here: an empty signature will not become an implicit fallback page.

`RecognitionCatalog::create` then performs cross-resource closure validation:

- recognizers and pages are both sorted by UUID;
- IDs and names are unique within their respective categories, and are also globally unique across
  pages and recognizers;
- a page signature may only reference existing `PageAnchor`s;
- each `allowedPageIds` entry must point to an existing page;
- no two pages may have exactly the same required/forbidden sets.

After successful construction, `RecognitionCatalog` also stores a deduplicated, UUID-sorted
`pageAnchorOrder`. Both page recognition and `PageResolver` use it as the single anchor evaluation
order, so the input container order does not leak into runtime results.

`recognizers()`, `pages()`, and `pageAnchorOrder()` return read-only `std::span`s; `findRecognizer()`
and `findPage()` return non-owning pointers. These views/pointers are all upper-bounded by the
catalog's lifetime, and the declarations make the constraint explicit with `UF_LIFETIME_BOUND` or a
comment.

### Canonical Authoring Document

`modules/annotation/source/annotation/authoring-document.hpp` defines the schema
`umbraflow-authoring/v1`. The main types are:

- `AuthoringSource`: a stable `SourceId`, a `ContentHash`, the derived `assets/sources/<hash>.png`
  path, a `ProjectFingerprint`, and a `SourceProvenance`.
- `WgcSourceProvenance`: a `TargetGeneration` and a canonical RFC 3339 `capturedAt`;
  `ImportedSourceProvenance` carries no fabricated WGC fields.
- `AuthoringRecognizerSpec`: a validated `RecognizerDefinition` plus its `SourceId`.
- `RegressionCase`: stores `RegressionClassification` and `RegressionExpectation` independently;
  positive/negative/confusable does not silently alter the resolved/unknown/ambiguous expectation.
- `AuthoringDocument`: owns the `RecognitionCatalog`, sources, recognizer-source relationships, and
  regressions.

`AuthoringDocument::create` is the document-level validation gate. It sorts sources, recognizers, and
regressions by UUID, requires that every source fingerprint equal the project fingerprint, closes the
annotation→source, regression→source, and resolved regression→page references, and guarantees that
source, recognizer, page, and regression IDs are globally unique. Each category table holds at most
4096 entries, and the canonical serialization result is at most 16 MiB.

Serialization and parsing live in `modules/annotation/source/annotation/authoring-document.cpp`. The
underlying `detail::CanonicalTomlReader` and append helpers are in
`modules/annotation/source/annotation/detail/canonical-toml.hpp` and
`modules/annotation/source/annotation/detail/canonical-toml.cpp`. It is not a general-purpose TOML
parser but a narrow grammar for the S0 generated format:

- every line must terminate with LF, CR is rejected, and the file naturally requires a trailing
  newline;
- fields and tables must appear in the writer's fixed order, and there is no entry point for unknown
  fields, comments, or extra trailing content;
- unsigned integers accept only the decimal canonical spelling, and the array separator is fixed as
  `", "`;
- strings accept only single-line basic-string syntax, valid UTF-8, and the canonical escapes the
  writer supports;
- after constructing the object, the parser re-invokes `serializeAuthoringDocument` and accepts only
  if the bytes are exactly equal.

This "parse → validate → serialize → byte compare" loop is the key to round-trip discipline. It
rejects not TOML semantics themselves but any representation that deviates from the GUI's single
output; thus generated/source documents can be hashed stably, diffed stably, and will not silently
absorb schema drift.

### Deterministic Compilation and the Runtime Manifest

`compileAuthoringDocument` lives in `modules/annotation/source/annotation/authoring-compiler.hpp` and
`modules/annotation/source/annotation/authoring-compiler.cpp`. Its inputs are a validated
`AuthoringDocument` and the one-to-one corresponding `AuthoringSourceAsset` PNG bytes, and its output
is a `CompiledAuthoringProject`:

- a `RuntimeManifest`;
- the canonical `runtimeManifestToml`;
- a deduplicated, path-sorted set of `TemplateAsset`s.

The compilation order is deterministic:

1. source assets are aligned to the document by `SourceId`, requiring the count and ID closure to
   match exactly.
2. unique crop tasks are established up front by source ID and `templateRect`; the same source/rect
   is done only once.
3. each source is first verified by SHA-256, then decoded and checked for width and height; only one
   source is decoded at a time.
4. `generateTemplateAsset`, via `modules/annotation/source/annotation/template-asset.cpp`, calls
   `image::cropBgra8`, `image::bgra8ToRgba8`, and `image::encodeRgbaPng`, then finally calls
   `sha256` on the encoded PNG bytes.
5. the generated path is fixed as `assets/templates/<hash>.png`; identical bytes with the same hash
   keep only one asset, while the same hash with different bytes is rejected.
6. recognizers are re-associated with the generated hashes, and a `RuntimeManifest` is created and
   canonically serialized.

The compiler uses checked arithmetic before allocation and processing. The current implementation
limits the total source pixels plus unique template-task pixels to no more than 256 Mi-pixels, and
the total unique generated template PNG bytes to no more than 512 MiB; each individual PNG is further
constrained by the image codec's file quota. A quota or closure failure returns `InvalidResource`,
and the function returns only a complete value, publishing no partial result.

#### Colour keys and the template mask

An `Element` may carry an optional `ColourKey` — a picked colour plus a tolerance — and the
compiler bakes it into the template's **alpha channel**. There is no new asset kind, no new manifest
field, and no separate mask file, because **the template PNG's alpha channel is the mask the matcher
weights by**: templates were already 32-bit with alpha pinned at 255 and unread, so writing that
channel was the whole change. `applyColourKeyAlpha` in `authoring-compiler.cpp` rewrites the fourth
byte of each cropped BGRA pixel with `ColourKey::alphaFor(red, green, blue)` and hands the result to
the same `generateTemplateAsset` as a whole image.

Distance is the sum of the three absolute channel differences, so it runs 0..765 and one unit is one
level on one channel. `alphaFor` gives full weight out to the tolerance and then **ramps linearly to
nothing at twice it**. The ramp is not decoration: a hard cut makes an author's tolerance control
jump in steps, and it cuts through the antialiased skirt of a glyph, where the pixels just past the
cut are still mostly glyph — on the measured menu entry a tolerance of 12 around the white text
takes 93.9% of the glyph and leaves a rim of edge pixels at distance 13..24, and those are what the
ramp readmits at the weight they deserve. A tolerance of 0 has no ramp and stays an exact-colour
mask; the maximum tolerance admits every colour, which is legal and useless — the same mask as no
key at all.

Three properties are load-bearing:

- **What is stored is the key, never the mask it produces.** An author reopening a project has to be
  able to move the tolerance and watch the selection change, and a baked mask cannot be moved back.
- **An element with no colour key emits byte-identical bytes** through the original call, so
  re-saving an existing project moves nothing, and every template authored before keys existed keeps
  a fully opaque alpha.
- **The derived catalog carries no colour key.** The key is authoring truth and the runtime reads
  the mask off the template's alpha, so the compiler takes it from the element the recognizer was
  derived from. Two elements that key the same rectangle of the same screen differently are two
  template tasks, so the crop/hash deduplication keys on the colour key as well.

`ContentHash` and the built-in SHA-256 are implemented in
`modules/annotation/source/annotation/content-hash.hpp` and
`modules/annotation/source/annotation/content-hash.cpp`. The text form is strictly `sha256:` followed
by 64 lowercase hexadecimal characters.

`RuntimeManifest` lives in `modules/annotation/source/annotation/runtime-manifest.hpp`. It owns a
`RecognitionCatalog` and, aligned by recognizer, a `RuntimeRecognizerAsset`: `templateHash`,
`sourceHash`, and `templatePath`. The runtime schema is `umbraflow-annotations/v1`, which contains no
source ID, capture time, target generation, or full screenshot.

`RuntimeManifest::create` validates that each recognizer has exactly one asset and derives the
template path from the hash as `assets/templates/<hash>.png`. `parseRuntimeManifest` uses the same
canonical reader, requires recognizers to precede pages, at most 4096 entries per category, and at
most 16 MiB per document, and at the end rejects non-canonical input via
`serializeRuntimeManifest(manifest) == input`.

### Thresholds, Runtime Recognition, and Stop Reasons

`SimilarityThreshold` in `modules/annotation/source/annotation/resource.hpp` stores `[0, 10000]` in
basis points. `maximumSad(width, height)` is implemented with checked `uint64` arithmetic:

```text
maxSad = floor((10000 - basisPoints) * 255 * width * height / 10000)
hit    = sadScore <= maxSad
```

**The threshold knows nothing about the mask, and that is a trap worth naming.** `maximumSad` is
derived from the template's *total* pixel count, while a masked score is rescaled to that same full
rectangle, which is exactly what keeps the two on one scale. But it means a key selecting a handful
of pixels is scored against a ceiling sized for the whole rectangle, and nothing fails: a 27-pixel
mask under a threshold sized for 920 pixels hits every frame and measures nothing. An element that
hits every state it is meant to distinguish is worse than no element, because it looks green. A
colour-keyed template therefore needs a far tighter threshold than an opaque one — 9000 basis points
gave three false-positive recognizers out of nine on the live game, and re-authoring at 9900 gave
7/7 on an unseen frame and 0/7 on a UI-free one. There is no authoring-time check for either
failure; `umbra-authoring frames probe` and its `fully_selected_pixels` are the only guard. See
`docs/pitfalls/colour-key-annotation.md`.

A key that selects *no* pixels is accepted at authoring time and aborts at match time with
`InternalInvariant`, which reads as "the program is broken" when the truth is "this key matches
nothing inside that rectangle".

The equality hit is part of the contract. `AnchorEvaluation::fromSadOutcome` lives in
`modules/annotation/source/annotation/recognition.cpp`; it checks that the matcher's returned
rectangle is still within `searchRoi` and that the score does not exceed the theoretical maximum SAD,
then constructs `AnchorEvidence`. `displayConfidence` is a floating-point display value, while `hit`
is determined solely by the integer `sadScore <= maximumSad`.

`RecognitionRuntime`'s public surface is in
`modules/annotation/source/annotation/recognition-runtime.hpp`:

- `create` accepts a `RuntimeManifest` and `EncodedRuntimeTemplate`s. It requires the received hash
  set to exactly equal the unique template set referenced by the manifest, recomputes each PNG's
  SHA-256, decodes into owned Gray8 bytes, and checks the template size against the recognizer
  geometry. It also extracts each template's **alpha channel as a mask plane**; the mask is left
  empty when every pixel is opaque, which selects the unmasked matcher and therefore the exact
  behaviour projects authored before masks had.
- `evaluatePage` returns a `PageRecognitionAttempt`, which may carry either a complete `PageOutcome`
  or a `PageRecognitionStop`, while preserving the anchor evidence already completed and the pixel
  comparison count.
- `recognizePage` is an operational convenience entry point: a completed outcome is returned as is,
  and a stop is mapped to a structured `Error`.
- `evaluateActionTarget` accepts only an `ActionTarget` from the catalog and returns an
  `ActionTargetAttempt`; a miss is `AnchorEvidence{hit=false}`, not an error, and a stop is still an
  explicit variant.

`RecognitionPolicy` carries a global pixel-comparison budget, an optional absolute deadline, and a
`std::stop_token` together. The runtime captures the cancellation and deadline by value into a
`SadSearchPoll`, then calls the bounded overload in `modules/vision/source/vision/sad.hpp`. Page
evaluation advances by `pageAnchorOrder` and deducts the completed comparisons from the remaining
budget; when any matcher returns `Cancelled`, `TimedOut`, or `ComparisonBudgetExhausted`, it returns
a stop immediately and never interprets it as a forbidden-anchor miss.

When the frame is Gray8, it establishes a synchronous read-only view directly; when it is Bgra8, it
converts once and calls the matcher while the backing vector is alive. `RecognitionRuntime` owns the
manifest and the decoded gray templates itself, so the externally supplied encoded buffers bear no
lifetime obligation after `create` returns.

`ensureCompatibleFrame`, before every page/action recognition, checks that the live fingerprint and
the manifest fingerprint are equal, and checks that the frame width/height equal the project base
resolution. A mismatch returns `TargetCompatibilityUnverified`; S0 has no implicit resampling.

### Page Resolution, Click Pixels, and Action Authorization

The page evidence types are in `modules/annotation/source/annotation/recognition.hpp`:

- `AnchorEvidence` records the recognizer ID, hit, optional SAD score, `maximumSad`, optional matched
  rect, and display confidence.
- `PageEvaluation` stores a page's required/forbidden evidence and its candidate flag.
- `PageResolutionEvidence` owns the project ID, `FrameIdentity`, all page evaluations, and the
  complete candidate IDs.
- `ResolvedPage`, `UnknownPage`, and `AmbiguousPages` all own evidence; only `ResolvedPage`
  additionally carries a unique `PageId`.

`PageResolver::resolve` requires the input to cover every anchor in `pageAnchorOrder` in exactly the
same order. It evaluates all pages: a page is a candidate only if every required anchor hits and
every forbidden anchor misses. Zero candidates return `UnknownPage`, one returns `ResolvedPage`, and
multiple return `AmbiguousPages`. It does not succeed early upon encountering the first candidate, so
a later conflicting page cannot be hidden by ordering.

`resolveClickPixel` lives in `modules/annotation/source/annotation/recognition-runtime.cpp`. For an
`ActionTarget`:

- when a `defaultClick` is present, it uses checked addition to add the template-local offset to the
  matched rect origin;
- when no offset is present, it takes `origin + extent / 2`, with integer division truncating
  downward, thereby selecting a unique pixel for both odd and even sizes.

Action authorization is defined in `modules/annotation/source/annotation/authorization.hpp` and
`modules/annotation/source/annotation/authorization.cpp`. `ActionDetection::create` does not trust the
string label: it requires the recognizer to genuinely be an `ActionTarget` in the catalog, the label
to equal that recognizer's name, and binds the `ProjectId`, `ElementId`, and an owned `Detection`
into a single value.

`authorizeCoordinateAction` implements a four-condition gate and continues to perform closure
validation at each layer:

1. **Live compatibility condition**: `ActionDeliveryState::m_liveFingerprint` must equal the catalog
   fingerprint, otherwise `TargetCompatibilityUnverified`.
2. **Same-project ResolvedPage condition**: both the page evidence and the action detection must
   belong to the active project, and the resolved page must still exist in the active catalog.
3. **Same-frame ActionDetection condition**: the recognizer must still be an `ActionTarget` in the
   active catalog, its `allowedPageIds` must include the resolved page, and the
   session/generation/frame triples of the page evidence, detection, and delivery must be exactly
   identical.
4. **Valid ObservationLease condition**: finally, it calls `ObservationLease::validate` to re-verify
   the session, target generation, frame ID, and expiry at the delivery moment.

The return value is only a `Status`, with no "partial authorization." `UnknownPage` and
`AmbiguousPages` cannot be passed as arguments by type, and a recognition stop never produces a
`ResolvedPage`, so a failure state cannot be mistaken for a clickable capability.

## Constraints That Must Remain True

**Fail-closed.** All external data is constructed through `Result` factories, and any failure of
schema, resource closure, hash, geometry, fingerprint, or quota produces no half-valid object. If the
page is not unique there is no `ResolvedPage`; a matcher stop is not a miss; if any action-
authorization condition fails there is no delivery permission. This is the structured implementation
of "rather not click than click wrongly on an unknown page," and it does not rely on the caller
remembering extra boolean checks.

**Determinism.** Stable UUID sorting governs the canonical bytes, template task order, anchor
evaluation order, and candidate order alike. Thresholds use basis points and integer-inclusive SAD;
the default click uses an integer offset or a truncated center. The template hash covers the actual
encoded PNG bytes, and the manifest path is uniquely derived from the hash. The same document and
source bytes therefore produce the same manifest, template bytes, evidence ordering, and click pixel.

**Explicit ownership and lifetime.** Document, catalog, manifest, runtime, evidence, and action
capability are all owning values; `RecognitionRuntime` owns the decoded templates. The public
spans/pointers are read-only borrows of the catalog/document's internal storage and must not outlive
the owner. A temporary `GrayImage` borrows the frame or a local conversion buffer only during the
synchronous matcher call; the poll captures copies of the token/deadline and holds no dangling
reference. `ActionDetection` owns the `Detection`, avoiding storing only a possibly colliding label
or borrowing caller state.

**Bounded work.** The authoring/runtime TOML, per-category records, compilation pixels, generated
template bytes, PNG inputs, and SAD comparisons all have explicit upper bounds. The deadline enters
the same matcher call together with cancellation and the comparison budget; this lets Preview,
regression, and Runtime share stop semantics, rather than setting up inconsistent timeout rules at
the UI layer.

**Strict-background evidence chain.** annotation does not touch the input API, but it requires the
page, detection, lease, and delivery fingerprint/identity to hold simultaneously.
`modules/engine/source/engine/session.cpp`, after authorization, further calls
`IFrameSource::validateTargetInstance`, passes the lease to `IActionSink::click`, and invalidates the
observation after successful delivery. The annotation gate is therefore the domain layer of a
two-layer delivery fence, not the entirety of a complete background protocol.

## Dependencies

The inbound authoring path comes from `entry/workbench`:

- `source-ingestion.cpp` turns an imported PNG or WGC frame into an `AuthoringSourceSpec` and an
  `AuthoringSourceAsset`.
- `authoring-edit.cpp` implements edit/undo/redo on an owning `AuthoringDocument` snapshot.
- `preview.cpp` calls `compileAuthoringDocument`, `RecognitionRuntime::create`, `evaluatePage`, and
  optionally `evaluateActionTarget`, so Preview has no private matcher.
- `project-persistence.cpp` compiles fully first, then publishes the immutable sources/templates,
  atomically replaces `annotations.toml`, and finally uses `generated/annotations.runtime.toml` as
  the runtime commit point.

The outbound runtime path enters engine:

- `modules/engine/source/engine/runtime-loader.cpp` reads the canonical runtime manifest and the
  unique template PNGs it references, and hands them to `RecognitionRuntime::create` to re-verify the
  hash closure.
- `modules/engine/source/engine/session.cpp` constructs a `RecognitionPolicy` from config and
  resolves the page and finds the action on the same `Observation` frame.
- an action hit is converted into a domain `Detection`, then bound to the recognizer identity via
  `ActionDetection::create`; `resolveClickPixel` produces the frame pixel, and
  `authorizeCoordinateAction` validates the four-condition gate at act time.
- engine converts the pixel into `FrameSpace`/`ClientSpace`, re-verifies the target instance, and
  only then calls the controller-backed `IActionSink`. What crosses the annotation boundary is values,
  evidence, and `Status`, not OS handles.

`runAuthoringRegressions` lives in `modules/annotation/source/annotation/regression-runner.cpp`. It
first goes through the real compiler/runtime construction path, then builds each source PNG into a
frame and calls `evaluatePage` one by one. The comparison budget restarts per case; cancellation and
the absolute deadline span the suite. A cancel/timeout interrupts subsequent cases, while a
single-case budget exhaustion is preserved as that case's diagnostic and allows the suite to
continue.

## Tests

`tests/annotation` is a deterministic offline test surface; the fixed contract of each file is as
follows:

- `test-catalog.cpp`: UUID/name canonical form, basis-point boundaries, recognizer geometry/page
  membership, empty/duplicate/contradictory signatures, cross-resource closure, and duplicate
  signatures.
- `test-authoring-document.cpp`: full byte-stable round trip of the authoring document, rejection of
  schema/order/integer/path/RFC 3339/CRLF drift, and the independence of regression classification
  and expectation.
- `test-runtime-manifest.cpp`: the runtime-only field set, fixed writer bytes, string escapes,
  non-canonical numbers, wrong template path, unknown kind/field, CRLF, and trailing-comment
  rejection.
- `test-content-hash.cpp`: single-block/multi-block standard vectors for SHA-256 and the lowercase
  canonical hash parser.
- `test-template-asset.cpp`: exact crop, PNG encode, content address, and out-of-bounds crop
  rejection.
- `test-authoring-compiler.cpp`: deterministic compilation after save/reopen, source relationships,
  same-crop/hash deduplication, the 256 Mi-pixel work boundary, missing/tampered source closure, and
  decode geometry.
- `test-recognition.cpp`: the `sadScore == maxSad` hit, the absence of priority among
  Resolved/Unknown/Ambiguous, and all three matcher stops terminating full page resolution.
- `test-recognition-runtime.cpp`: Gray8/Bgra8 evidence equivalence, page outcome, global budget,
  cancel, deadline, fingerprint/template closure, shared templates, action hit/miss/type checks,
  action stop, and `resolveClickPixel`.
- `test-authorization.cpp`: the authorization gate composed of fingerprint, same-frame identity,
  recognizer-label binding, allowed page, active catalog/project, and lease.
- `test-regression-runner.cpp`: resolved/unknown/ambiguous evidence, expectation mismatch, suite
  interruption, per-case budget, and source reuse.
- `test-helpers.hpp` provides only strongly typed fixtures/builders, not a second set of production
  logic.

When modifying the schema writer/parser, sorting, thresholds, or stop propagation, you should at
least check both the "construction validation tests" and the "full compiler/runtime tests." Adding
only parser unit tests is insufficient to prove that Preview and Runtime still take the same path;
adding only a runtime happy path is likewise insufficient to prove that non-canonical input remains
fail-closed.

## Future Extensions

The following seams come from the authoritative plan and are not currently implemented capabilities.

**P1 resolution adaptation.** `docs/plans/2026-07-22-annotation-design.md` §2 and
`docs/plans/2026-07-21-lua-task-model-grill-decisions.md` D8 require adding an explicit
`BaseToLiveTransform { uniformScale, offset, viewport }`. It should sit at the boundary where base
annotation geometry enters live frame search/click geometry, and it should fully enter the trace; it
must not modify the existing live `CoordinateTransform`'s Client↔Frame responsibility, nor scatter
raw scale into the recognizer.

**New recognizer kind.** `RecognizerDefinition` already centralizes type, geometry, threshold, and
page membership, and `RecognitionRuntime` further isolates the decoded template as an internal
`GrayTemplate`. When adding color, OCR, or composite, the seam runs through the schema version/parser,
catalog validation, compiler asset closure, runtime-owned kernel, and evidence; you cannot merely add
a UI option in workbench. The authoritative plan §7 states clearly that P0 allows only
`gray_template`, and OCR may be adjudicated early only when real daily use genuinely requires reading
dynamic semantics and a template/state anchor cannot express it.

**`InfoRegion` evaluation.** The enum and the authoring/runtime schema can already express
`InfoRegion`, but the current `AnchorEvaluation::fromSadOutcome` accepts only
`PageAnchor`/`ActionTarget`, and the public runtime has no `evaluateInfoRegion`. A future
read-oriented API should add a separate result type in `RecognitionRuntime`, reusing the same policy,
hash closure, and evidence rules, rather than masquerading as an authorizable action.

**Page diagnostics, not heuristic disambiguation.** `PageResolutionEvidence` already retains all page
and candidate evidence and can support the page confusion diagnostics of
`docs/plans/2026-07-21-product-form-and-roadmap.md` P1. Extensions should consume this evidence to
improve the authoring experience; the S0 authority forbids adding priority, threshold override, or
heuristic tie-break in the runtime, so diagnostics must not change the fail-closed semantics of
`Ambiguous`.

**P1/P2 host surface.** Luau opaque handles, popup interrupt, the P2 no-activate overlay, and HTML
trace are all outside annotation. They should consume `RecognitionCatalog`'s stable IDs/names,
`PageResolutionEvidence`, `AnchorEvidence`, and stop reason. In particular, the overlay may display
evidence but must not become a new matcher or bypass `authorizeCoordinateAction`.

Any work that changes the canonical bytes, schema fields, threshold decisions, page outcome, or the
four-condition authorization gate is not a local refactor: it changes the S0 shared contract. Before
implementing, you should first update the authoritative decisions in
`docs/plans/2026-07-22-annotation-design.md`, then synchronize the authoring writer/parser, runtime
loader, workbench Preview, engine integration, and the test matrix above.
