# vision and image Module Architecture Knowledge

This document describes two small platform-independent modules together: `modules/vision` provides
deterministic pixel recognition, while `modules/image` owns resource-bounded PNG encoding/decoding
and pixel layouts. Authoring, Preview, and Runtime use both modules, but the two do not depend on
one another. The related S0 design is in `docs/plans/2026-07-22-annotation-design.md`.

## Module Responsibilities

`vision` owns five things:

1. Deterministically converting a strided BGRA8 frame into tightly packed Gray8 bytes.
2. Performing SAD (sum of absolute differences) template search within an integer `PixelRect`, and
   yielding a stable best position and a `uint64` score — optionally **weighting each template pixel
   by a parallel mask plane** (2026-07-30, `c392161`).
3. Providing a precise comparison budget for long searches, a synchronous cancellation/deadline
   poll, and a stop reason that is never lost.
4. Owning the colour-key weight rule: `colourKeyAlpha` in
   `modules/vision/source/vision/frame-analysis.hpp` is its one implementation, and
   `annotation::ColourKey::alphaFor` delegates here.
5. Answering three measurement questions about a **set** of decoded frames — stability, colour probe,
   colour census — in `modules/vision/source/vision/frame-analysis.hpp` (2026-07-30, `eacb05f`).

`image` owns the other three things:

1. Decoding PNG bytes or files into owned, tightly packed RGBA8 pixels in the `uf::image`
   namespace, and performing the reverse encoding.
2. Explicitly converting RGBA8/BGRA8 layouts, and deterministically cropping a tightly packed
   rectangle out of a strided BGRA8 source.
3. Confining stb's C API, foreign allocation, raw pointers, and process-global encoder
   configuration inside `modules/image/source/image/ffi/`.

Both `manifest.txt` files publicly depend on `core` and `domain`; `image` additionally has a
private `image_stb` dependency. See `modules/vision/manifest.txt`, `modules/image/manifest.txt`,
and `modules/image/external/CMakeLists.txt`. The module graph has no `vision -> image` or
`image -> vision`:

- `vision` does not need to know whether a static template comes from PNG, WGC, or a test fixture;
  it only receives an already-validated pixel view.
- `image` does not need to know whether the pixels will be used for recognition, a canvas texture,
  or capture evidence; it only performs codec and layout work.
- When "PNG → Gray8" is needed, the `annotation` or entry layer explicitly composes `decodePng`,
  `rgba8ToBgra8`, and `bgra8ToGray8`. This explicit bridge keeps codec/FFI concerns from polluting
  the pure recognition kernel.

They deliberately do not own the following policies:

- No window discovery, no frame capture, no holding of OS handles, and no sending of `PostMessage`;
  these belong to the controller and entry adapters.
- No defining of similarity thresholds, page signatures, required/forbidden anchors, `ResolvedPage`,
  or action authorization; these belong to `annotation`.
- No computing of `ContentHash`, no deriving of `assets/templates/<hash>.png`, and no validating of
  the runtime asset closure. `image` only guarantees encoded bytes; `annotation` decides the
  identity meaning of those bytes.
- No atomic publishing of an authoring project. `writeRgbaPng` is a file function that directly
  performs open/truncate/write/flush/close; the commit point and rollback discipline live in
  `entry/workbench/project-persistence.cpp`.
- No scaling, resampling, color recognizer, OCR, composite recognizer, or multi-scale search.
- No implementation of strict-background input. The contribution of these modules to that product
  contract is: invalid images are rejected and incomplete searches are preserved as a stop, so the
  upper layers have no basis to turn incomplete evidence into a background input.

## Image Processing and Recognition Flow

### The Public Interface of vision

The public declarations live in `modules/vision/source/vision/sad.hpp`.

`GrayImage` is a read-only Gray8 view that does not own the pixels. `GrayImage::create` accepts a
span, `width`, `height`, and `stride`, and reuses `validateBufferGeometry` from
`modules/domain/source/domain/frame.cpp` to reject zero dimensions, a short stride, multiplication
overflow, or insufficient backing storage. On success it stores only the span and geometry, so the
backing owner must cover the lifetime of the view and of all synchronous matcher calls.

`bgra8ToGray8` accepts the same strided geometry, validates that each row is at least `width * 4`
bytes, and then performs, pixel by pixel:

```text
gray = (77 * red + 150 * green + 29 * blue) >> 8
```

The implementation lives in `modules/vision/source/vision/sad.cpp`. It ignores alpha, skips input
row padding, and returns an owning vector of exactly `width * height` bytes. The integer weights
and right shift fix the truncation behavior; for example, pure red, green, and blue yield 76, 149,
and 28 respectively, rather than depending on platform floating point or a color library's rounding.

`SadMatch` holds `x`, `y`, and `score`. A smaller score is better, and zero means a pixel-for-pixel
exact match. `vision` itself has no threshold: as long as the template can be legally placed within
the ROI, a completed search returns the best `SadMatch`; an empty `std::optional<SadMatch>` only
means that there was no legal candidate, not that the "score exceeded a business threshold".

All three control types live in the same header:

- `SadSearchControl` is the return value of a poll: `Continue`, `Cancelled`, `TimedOut`.
- `SadSearchStopReason` is the stop result the matcher stores for the outside: `Cancelled`,
  `TimedOut`, `ComparisonBudgetExhausted`.
- `SadSearchPoll` is a `std::function<SadSearchControl()>` that is called synchronously and is never
  retained by the matcher.

`SadSearchOutcome` distinguishes `std::optional<SadMatch>` from `SadSearchStopReason`.
`SadSearchReport` additionally carries `m_completedPixelComparisons`; the count covers all
comparisons actually performed, including the one that triggers pruning or an exact-match return,
but not the next comparison that a budget/poll has already blocked.

`matchTemplateSad` has four overloads, two unmasked and two masked:

- The three-argument overload returns `Result<std::optional<SadMatch>>`, and internally uses the
  maximum `uint64` budget and a poll that always returns `Continue`.
- The bounded overload accepts `maximumPixelComparisons` and a `SadSearchPoll`, and returns
  `Result<SadSearchReport>`. Runtime, Preview, and the frozen m0-demo all use this overload.
- Two further overloads take a `templateMask` between the template and the ROI, in the unbounded and
  bounded shapes respectively. They are **additive**: every existing call is untouched.

#### The masked matcher

The mask is a Gray8 plane with the template's exact extent. A mask byte of 255 counts its pixel in
full, 0 excludes it, and an intermediate value is a **partial weight**, which is what an antialiased
glyph edge needs. The reported score normalizes by the weight actually summed and rescales to the
template's full rectangle:

```text
score = templatePixels * sum(weight * |haystack - template|) / sum(weight)
```

That rescaling is the point. A mask covering a tenth of its rectangle stays on the same scale as one
covering all of it, and therefore on the scale existing unmasked thresholds already use, so
`SimilarityThreshold` did not have to learn about masks. Truncating division rounds the quotient
down. **A mask whose weights sum to zero selects nothing and is rejected rather than divided by.**

Two properties hold the old behaviour. The overloads are additive, so no existing call changed; and
**a fully opaque mask is bit-identical to the unmasked matcher** — same score, same match, same
prune decisions, same comparison count — because the constant 255 cancels out of the quotient and
out of every pruning comparison. That was verified by forcing every template down the masked path
and watching nothing change.

Colour is not the matcher and must not be. On the measured menu, bright white covers 8.2% of the
menu region but 10.9%–18.7% of the artwork behind it: keyed on colour alone, the character art
outranks the menu. **The colour selects which pixels are compared; SAD still identifies the shape.**

`bgra8ToAlpha8` extracts a BGRA8 buffer's alpha channel as a tightly packed Gray8-shaped plane,
which is the mask the masked matcher consumes. A template PNG's alpha channel *is* that plane, which
is why colour keying needed no new asset kind — see `module-annotation.md`.

#### Frame analysis

`modules/vision/source/vision/frame-analysis.hpp` answers three questions about a set of frames,
each answered by hand once and each too slow to answer that way twice. Frames arrive already decoded
as `BgraImage` BGRA8 planes (`modules/vision/source/vision/bgra-image.hpp`); this module never
touches a file.

**They take N frames, not two.** `analyseStability` and `probeColour` each take one `std::span` of
frames rather than a pair, because the useful set is two backgrounds *plus a frame taken seconds
later to catch animation*: a pixel that is stable in the first two and moves in the third is not
stable, and that difference is invisible both to semantics and to the eye while producing
intermittent match failures. A two-frame API would have needed replacing immediately. Both require
at least two frames — one frame is stable everywhere and answers nothing.

- `analyseStability(frames, StabilitySpec)` reports which pixels of a rect held still across every
  frame. Its `stableMask` is 255 where every frame agreed and 0 elsewhere, shaped so it can be handed
  straight to `matchTemplateSad` as a template mask. `grayTolerance` is the grey spread a pixel may
  still be called stable at, and zero demands byte identity, which is what UI drawn over changing
  artwork actually produces. `minimumGap` splits a region on a run of fully unstable rows or columns;
  zero disables splitting and reports the naive single bounding box, which merges a menu label with
  the sub-label under it. It also returns the row and column profiles the regions were cut from, so a
  caller can choose `minimumGap` from the data rather than by guessing.
- `probeColour(frames, ColourProbeSpec)` measures how well one colour key isolates whatever holds
  still in a rect. It reports `fullySelectedPixels` and `rampSelectedPixels`, and the gap between
  `maskedMeanGraySpread` and `rectMeanGraySpread` is the whole answer to whether the rect survives a
  background change. `fullySelectedPixels` is also the only guard against a key that selects nothing:
  there is no authoring-time check for that.
- `censusColours(frame, ColourCensusSpec)` counts the colours of **one** frame's rect, so a key is
  picked from data instead of by sampling a pixel and hoping. Alpha is ignored — a captured frame's
  alpha carries no colour. It is single-frame and O(pixels), so unlike the two scans above it takes
  no budget. Equal counts are ordered by packed blue/green/red ascending, so the report never depends
  on traversal order.

The two scans share the matcher's budget and cancellation vocabulary — `SadSearchPoll`,
`SadSearchControl`, `SadSearchStopReason`, `k_sadSearchPollIntervalComparisons` — rather than
declaring a parallel set, and count `completedPixelVisits` (one per pixel per frame) the way the
matcher counts comparisons. The `Sad` prefix on those names is historical; one convention per module
is worth more than a better prefix.

`colourKeyAlpha(pixel, keyRed, keyGreen, keyBlue, tolerance)` is the colour-key weight rule's **one**
implementation: full weight out to the tolerance, then a linear ramp to nothing at twice it, with a
tolerance of zero staying an exact-colour match. Distance is the sum of the three channel distances,
so `k_maximumColourKeyTolerance == 765` is the widest tolerance that means anything. It lives here
rather than in `annotation` because `probeColour` needs it to answer how many pixels a key selects,
which is the question that function exists for; `annotation::ColourKey::alphaFor` delegates here, and
the dependency runs annotation → vision so the call can only go that way. Before 2026-07-30
(`eacb05f`) there were two byte-identical copies of the rule, and that drift would have been silent:
authoring would bake one mask while the probe reported another.

The bounded search first calls `roi.ensureWithinExtent`. Legal candidates are traversed in
row-major order with `candidateY` as the outer loop and `candidateX` as the inner loop, accumulating
absolute differences row by row. The differences are non-negative, so once the partial sum after a
row is already `>= best` the search can safely prune. Only `score < best` replaces the result, so a
tie keeps the earliest position; a zero score returns immediately, because no better score can exist.

Both the budget and the poll are checked "before performing the next pixel comparison". The order is
to check the budget first, then poll when the cumulative count is `0, 4096, 8192, ...`; the interval
constant is `k_sadSearchPollIntervalComparisons == 4096`. Therefore:

- when the budget is zero, it returns `ComparisonBudgetExhausted` directly, without calling the
  poll, with a count of zero;
- the poll can report an immediate cancel/timeout before the first comparison;
- after exactly 4096 comparisons have been performed, if budget remains, the poll runs again before
  the next comparison;
- when the budget is exhausted exactly, the stop does not count the unperformed comparison into the
  report.

`modules/vision/source/vision/synthetic.hpp` additionally exposes `hashedGray`. It generates a
`uint8` from `seed`, `x`, and `y` using a fixed unsigned integer multiply/xor sequence; its current
caller is the deterministic synthetic fixture in `tests/vision/test-sad.cpp`. It is not a PNG hash
or a content-identity algorithm.

### The Public Interface of image

The PNG API lives in `modules/image/source/image/png.hpp`. The three public quotas are:

- `k_maximumPngDimension == 8192`: the per-axis upper bound;
- `k_maximumPngPixels == 8192 * 8192`: the total pixel upper bound;
- `k_maximumPngFileBytes == 64 * 1024 * 1024`: the upper bound on encoded decode/load input.

`RgbaImage` owns `m_width`, `m_height`, and `m_pixels`. When returned by the decoder, the pixels are
always tightly packed RGBA8 of `width * height * 4`, and the PNG's original color type does not leak
to the caller.

The input to `decodePng` is a synchronously borrowed encoded span and a `resourceName` used only for
diagnostics. The implementation is in `modules/image/source/image/ffi/png-decoder.cpp`, and before
entering stb it performs, in order:

1. Rejecting empty input and encoded bytes exceeding 64 MiB.
2. `validatePngStructure` checks the PNG signature, that the first chunk must be a length-13 IHDR,
   that IHDR must not repeat, that every declared chunk length falls within the input, and that IEND
   must be empty and end the file exactly.
3. Pre-reading width, height, and bit depth from IHDR, and rejecting zero dimensions, axis quota,
   pixel quota, and checked-size overflow.
4. Pre-allocating the exact RGBA8 destination, then handing the bounded span to stb.

The decoder defines `STBI_ONLY_PNG`, `STBI_NO_STDIO`, and `STBI_NO_FAILURE_STRINGS`. Ordinary input
calls `stbi_load_from_memory` and forces `STBI_rgb_alpha`; a 16-bit PNG calls
`stbi_load_16_from_memory`, with each sample round-to-nearest via `(sample * 255 + 32767) / 65535`.
The foreign allocation immediately enters a `std::unique_ptr` with `Stbi8ImageDeleter` or
`Stbi16ImageDeleter`, and the stb pointer does not cross the function boundary.

`loadPng` first runs a 64 MiB preflight with `std::filesystem::file_size`, then reads the exact bytes
and calls `decodePng`. A missing, unreadable, short-read, or malformed template all return
`InvalidResource`. The current parser and stb do not verify PNG chunk CRC; this is the explicit
reserved boundary that `docs/plans/2026-07-20-m0-demo-port-deviations.md` F-14 draws for "trusted
template input", and the structural preflight must not be described as a complete cryptographic
integrity check.

`encodeRgbaPng` and `writeRgbaPng` are implemented in
`modules/image/source/image/ffi/png-encoder.cpp`. The encoder rejects zero dimensions, axis/pixel
quota overruns, stb signed geometry that is not representable, and non-tight RGBA input that does not
equal `width * height * 4`. It also constrains `(rowBytes + filterByte) * height` with the private
`k_maximumFilteredPngBytes`, and uses `static_assert` to prove that this upper bound is well below
stb's signed working range. The 64 MiB quota is the upper bound on decode/load encoded input; the
encoder's own local working bound is made up of the dimension, pixel, and filtered-byte checks.

Encoding does not call stb file I/O; instead it uses `STBI_WRITE_NO_STDIO` and
`stbi_write_png_to_func`. `appendEncodedPng` appends each segment of callback bytes to the per-call
`EncodedPng::m_bytes`. The callback is `noexcept` and, for null, non-positive size, checked-size
overflow, or an allocation exception, only sets `m_callbackFailed`; no exception escapes across the
C callback boundary. stb's `STBIW_ASSERT` is replaced by the release-active `UF_CHECK_MSG`, so a
stretchy-buffer realloc failure terminates before it can continue writing out of bounds.

Before the first encode, a function-local magic static explicitly writes:

```text
stbi_write_png_compression_level = 8
stbi_write_force_png_filter = -1
```

These two values are explicitly frozen configuration, not incidentally inherited from mutable
process defaults. The static initialization guard makes the write happen only once and ensures that
concurrent callers synchronize before stb reads these non-atomic globals. There is no other setter
in the repository; the complete golden byte sequence in `tests/image/test-png.cpp` then forces
manual review whenever a stb implementation upgrade changes the output.

`writeRgbaPng` reuses `encodeRgbaPng` and then checks open, write, flush, and close step by step. A
filesystem failure returns `IoFailure` and preserves the native `std::error_code`; an stb/callback
encoding failure returns `ExternalFailure`.

The pixel API lives in `modules/image/source/image/pixels.hpp`:

- `rgba8ToBgra8` and `bgra8ToRgba8` both take the vector by value and swap the red/blue of each
  four-byte pixel in place on the original allocation; green and alpha are unchanged, and when the
  byte count is not a multiple of four they return `InvalidResource`.
- `cropBgra8` accepts a borrowed source, source geometry, stride, and a `PixelRect`. It checks the
  rect bounds, row/storage geometry, and all checked byte offsets, copies the valid BGRA bytes row
  by row, and returns a padding-free owning vector of `rect.width() * rect.height() * 4`.

### The Two Actual Data Chains

The authoring asset chain keeps "static cropping" and "runtime search range" as two distinct
operations:

```text
source BGRA8 + templateRect
  -> image::cropBgra8 -> image::bgra8ToRgba8 -> image::encodeRgbaPng
  -> annotation::sha256(encoded PNG bytes) -> content-addressed template
```

The runtime/Preview recognition chain is:

```text
template PNG -> image::decodePng -> image::rgba8ToBgra8 -> vision::bgra8ToGray8
live Frame BGRA8 -----------------------------------------> vision::bgra8ToGray8
two GrayImage views + independent searchRoi -> bounded matchTemplateSad
```

The template PNG hash covers the encoded bytes, not the decoded pixels. Therefore, if the same
pixels produce different PNG bytes under a different encoder/config, they will get a different
identity; the pinned encoder and golden test are part of the content-addressed contract, not merely
a compression-performance setting.

## Constraints That Must Remain True

**Determinism.** Gray conversion uses only fixed-width integer arithmetic; SAD uses only integer
absolute differences and `uint64` accumulation; candidate order, strict-less update, row pruning,
and exact-zero return fix the tie result. Channel swap, crop, and 16→8 bit conversion also have a
unique byte result. The PNG layer converges the same RGBA input to the same encoded sequence through
explicit stb globals, a per-call independent output buffer, and golden bytes. Any change to these
rules affects evidence, trace, or content hash, and must not be treated as a semantics-free refactor.

**Fail-closed.** Geometry, stride, buffer size, ROI, checked arithmetic, PNG structure, and quota
are all validated before any read or foreign allocation. When an FFI callback cannot safely append,
it only produces a failure and does not return a partial PNG; when a foreign decode pointer is null
or its dimensions disagree with the pre-checked IHDR, no image is returned. Internal "impossible"
states use the release-active `UF_CHECK`, while external resource errors use `Result`. The only
explicit exception is the current trusted-input CRC decision, which is recorded by a reservation plan
and must not be misrepresented as already validated.

**A search stop is not a miss.** `Cancelled`, `TimedOut`, and `ComparisonBudgetExhausted` all
indicate that not all necessary candidates were completed, so they can neither prove that "a best
match exists" nor that "there is no acceptable match". If a stop is collapsed into `hit=false`, a
required anchor will incorrectly yield Unknown, and, more dangerously, a forbidden anchor will help a
page become a candidate because of the "miss". The independent variant alternative in
`SadSearchOutcome` prevents this information loss starting at the kernel; `annotation` then maps the
three respectively to `Cancelled`, `Timeout`, and `RecognitionIncomplete`. That last kind is named
for the incompleteness, not for a failure to recognise: it says the search never finished looking, so
its `FailureResponse` is `Retry` and a caller must observe again instead of concluding anything about
the screen.

**Ownership and lifetime are explicit.** `GrayImage` is a short-lived borrow, and neither the
declaration nor the implementation stores the backing owner; the matcher and poll both complete
synchronously and do not retain the callback. The PNG decode/encode result, crop, and layout
conversion result are all owning vectors. FFI allocation uses a custom-deleter `unique_ptr`, and raw
pointers appear only in synchronous copies/views justified by a `// SAFETY:` comment. The
upper-layer `RecognitionRuntime` ultimately owns the decoded Gray8 template, so the original PNG and
the intermediate RGBA/BGRA buffers can be released after creation.

**Work is bounded and accountable.** Before calling stb, PNG limits the encoded bytes, axes, pixels,
and destination size; the encoder additionally limits the filtered working bytes. The SAD product
path has, simultaneously, a comparison budget, a cooperative poll every 4096 comparisons, and a
precise completed count. Pruning is a performance optimization, but it does not relax the budget/poll
and does not change the best result.

**Strict-background holds only indirectly, through the evidence chain.** These two modules have no
input capability and cannot promise background delivery to a window. What they guarantee is that what
they hand to the upper layers is either completed evidence or an explicit stop.
`modules/engine/source/engine/session.cpp` can call `IActionSink::click` only after annotation
produces authorizable completed evidence; a stop first enters the trace and returns an error. So what
is held here is the recognition precondition of strict-background, not the delivery protocol itself.

## Dependencies

In the downward dependencies, `core` provides `Result`, release-active contracts, checked
arithmetic/casts, and checked access; `domain` provides `PixelRect`, `PixelFormat`, `Frame`, and
`validateBufferGeometry`. What crosses this edge is only integer geometry, spans/vectors, and
structured errors — no stb types or platform handles.

`modules/annotation` is the most important shared consumer of the two modules:

- `modules/annotation/source/annotation/template-asset.cpp` calls crop, BGRA→RGBA, and PNG encode,
  then computes SHA-256 over the encoded bytes to produce a `TemplateAsset`.
- `modules/annotation/source/annotation/recognition-runtime.cpp`, in `RecognitionRuntime::create`,
  re-verifies the template hash closure, decodes the PNG, converts RGBA→BGRA→Gray8, and owns the
  final gray template.
- `withGrayFrame` in the same file builds a view directly over a Gray8 `Frame`, converts a BGRA8
  `Frame` only once, and guarantees that the local gray vector lives until the continuation returns.
- `RecognitionPolicy` is turned into a `SadSearchPoll` that captures the stop token and deadline by
  value; page evaluation also deducts from the same global comparison budget across multiple anchors.

`entry/workbench` consumes `image` directly:

- `source-ingestion.cpp` decodes an imported PNG and re-encodes it canonically, or removes padding
  from a WGC BGRA frame, converts it to RGBA, and encodes it; the source hash therefore covers the
  project's own canonical PNG bytes.
- `preview.cpp` goes through the real compiler/runtime path and does not maintain a private matcher.
- `platform/windows-texture-cache.cpp` only hands the RGBA output of `decodePng` to a D3D texture
  upload.
- `project-persistence.cpp` is where the file publication order and the encoded asset size gate live.

`modules/engine` does not depend on `image` or `vision` directly; it obtains recognition results
through its public dependency on `annotation` and serializes `SadSearchStopReason` in
`modules/engine/source/engine/trace.cpp`. This lets the engine see the recognizer evidence and stop
vocabulary but not the codec or the matcher's internal storage.

`entry/m0-demo` is a frozen real-hardware acceptance reference and still uses both modules directly:
load/convert template, crop/convert frame, and bounded SAD. Capture PNG output moved to
`entry/input-agent`, which owns the writer both programs call. Its direct calls
should not be treated as an extension point for new product policy; the current product path is
annotation + engine.

The controller only produces a `Frame` with a `PixelFormat`, stride, and an owning `FrameBuffer`. It
does not need to link against either module; the composition layer hands the frame to recognition.
There is no reverse edge either: the recognition kernel never calls capture or input.

## Tests

`tests/vision/test-sad.cpp` is the complete direct test surface of `test-vision`, pinning:

- exact hit, agreement with a brute-force exhaustive scan, row-major tie, padded stride, and exact
  ROI bounds;
- the counts for zero/one/exact comparison budget and exact-match early return;
- the three stops `Cancelled`, `TimedOut`, `ComparisonBudgetExhausted` and the 4096-comparison poll
  interval;
- the error kind for illegal `GrayImage` geometry and an out-of-bounds ROI;
- BT.601 integer samples, alpha ignored, input padding ignored, tight Gray8 output, and
  bad-geometry rejection;
- the masked matcher: that an opaque mask reproduces the unmasked result exactly, that a
  zero-weight mask is rejected rather than divided by, and the frame-analysis primitives against
  `tests/vision/label-fixture.hpp`, real menu-label pixels captured over two backgrounds. One of
  those numbers is a hard cross-check rather than an agreement — the summed score over the fixture
  rect is the same `opaqueScore` the masked-SAD test pins, so the two fixtures are provably the same
  rectangle.

`tests/annotation/test-colour-key-join.cpp` is the join test between the two halves, which were
built in parallel and each tested against a hand-built artifact until it existed. It puts
`compileAuthoringDocument`'s emitted PNG bytes straight into `RecognitionRuntime` with nothing
hand-built between: keyed, the label matches at 0.046 mean difference; unkeyed, the same rectangle
misses at 56.979 against a 25.5 threshold. It was falsified by repointing the key at the artwork,
which inverts the mask and puts the mean at 69.47.

`tests/image/test-pixels.cpp` pins RGBA/BGRA byte order, incomplete-pixel rejection, template/frame
sharing the same gray kernel, and the tight output of a padded crop along with short-source
rejection.

`tests/image/test-png.cpp` pins:

- the exact RGBA round trip of `writeRgbaPng` → `loadPng`;
- byte-identical repeated encoding of the same input;
- the complete PNG golden sequence of a 2×2 fixture, covering the header, IDAT, CRC bytes, and IEND;
- the `IoFailure`, native error category, and operation message of a write failure;
- encoder/decoder dimension quota, empty/non-PNG input, over-quota IHDR, and truncated chunk length;
- the round-to-nearest RGBA8 result of a 16-bit sample.

The cross-module contract continues to be pinned by the following tests:

- `tests/annotation/test-template-asset.cpp` pins stride-aware crop, channel order, repeated-encoding
  bytes, the concrete SHA-256, and the hash-derived path; if the encoder bytes change at all, the
  template identity test fails outright.
- `tests/annotation/test-authoring-compiler.cpp` pins deterministic compilation, deduplication of
  identical crop/hash, pixel-work bounds, and source hash/geometry closure.
- `tests/annotation/test-recognition.cpp` pins the inclusive integer threshold and proves that every
  matcher stop terminates page resolution.
- `tests/annotation/test-recognition-runtime.cpp` pins Gray8/BGRA8 evidence equivalence, the
  cross-anchor global budget, immediate cancel, expired deadline, template decode/hash closure, and
  action-target stop.
- `tests/annotation/test-regression-runner.cpp` pins the interruption of the suite by cancel/timeout,
  and the propagation of a budget stop as a per-case diagnostic.
- `tests/workbench/test-source-ingestion.cpp` pins import canonical re-encode, WGC provenance, the
  BGRA/RGBA bridge, and stride padding removal; `tests/workbench/test-preview.cpp` pins that Preview
  exposes a budget stop.
- `tests/engine/test-session.cpp` pins a budget/cancel stop as an error, zero clicks, and the
  `RecognitionStopped` trace; `tests/engine/test-trace.cpp` pins the JSONL spelling of the stop
  reason.

`tests/CMakeLists.txt` registers the CI-labeled `test-vision` and `test-image` and places
integration into each respective target. `tests/workbench/test-real-regression.cpp` is registered as
`REAL` only when the local `tests/assets/real-regression` exists, so that it serves future real
screenshots without putting game assets into CI.

When changing matcher traversal/pruning/poll, it is not enough to verify only the final coordinate;
the exact comparison count and the stop timing must also be verified. When changing codec/config, it
is not enough to do only a decode round trip; the golden bytes, template SHA-256, and authoring
compiler determinism must also be verified. The former protects control semantics, the latter
protects content identity.

## Future Extensions

`docs/plans/2026-07-22-annotation-design.md` §7 locks P0 to only a bounded deterministic
`gray_template`. Color, HSV, OCR, composite, parameterized ROI, and multi-scale are not hidden modes
of the current kernel. If the authoritative plan permits a new recognizer, it should add a parallel,
equally bounded kernel/result contract and synchronize the annotation schema, asset closure,
evidence, Preview/Runtime, and trace; new semantics must not be stuffed into `matchTemplateSad` while
still reusing the old stop/score meaning.

§2 of the same authority and `docs/plans/2026-07-21-product-form-and-roadmap.md` P1 reserve
resolution adaptation: the planned `BaseToLiveTransform { uniformScale, offset, viewport }` and
deterministic template resampling must be explicit steps between the base annotation geometry and the
live frame geometry. That type does not exist in the current code; the integration point is before
the annotation runtime assembles the live ROI/template and `GrayImage`, not a quiet change to the
existing `CoordinateTransform`'s Client↔Frame responsibility, and not letting the `image` decoder
guess DPI.

OCR may be re-adjudicated only when real daily use must read dynamic semantic text/numbers and a
template or state anchor cannot express it; see `docs/plans/2026-07-22-annotation-design.md` §7 and
product roadmap P1. At that point the PNG ingress and quota can still be reused, but the OCR output,
determinism, budget/cancellation, and evidence type all need independent design; a SAD score is not a
general confidence interface.

The seam for an stb upgrade or a codec replacement is exactly the two `ffi/*.cpp` files and the
private `image_stb` target; the public `image/png.hpp` need not leak third-party types. But
`docs/plans/2026-07-23-engine-architecture.md` Phase 1 states explicitly that the encoder
configuration must be frozen before real assets, because a byte drift invalidates every
`template_hash`. Any upgrade should first explain the `tests/image/test-png.cpp` golden diff, then
decide whether to migrate and regenerate the content-addressed source/template assets; it must not
merely update the golden constants.

The current CRC trade-off relies on a trusted template. If a future product begins to accept
downloaded or shared untrusted assets, `docs/plans/2026-07-20-m0-demo-port-deviations.md` F-14 should
be re-adjudicated first, and the corresponding validation should be added at the FFI
preflight/integrity boundary, rather than having every annotation/engine caller add its own check.

The ready-made seam for real-screenshot regression is `tests/workbench/test-real-regression.cpp` and
the conditionally registered `tests/assets/real-regression`.
`docs/plans/2026-07-23-engine-architecture.md` Phase 5 requires expanding the positive, negative, and
confusable sets with real sources produced by the workbench; the kernel and codec must not contain
any game-specific special cases.

Finally, changing the Gray formula, SAD traversal/tie, poll interval, PNG canonical bytes, quota, or
pixel layout may all cross the S0 shared contract. Before implementing,
`docs/plans/2026-07-22-annotation-design.md` should be reviewed and updated if needed, and then the
vision/image direct tests, the annotation content hash and recognition tests, workbench Preview, and
the engine stop trace should be synchronized; these are not local implementation details that can be
released in isolation.
