# P0-A Visual Annotation System & Data Model — Design Draft

> Status: **DRAFT for developer review** — 2026-07-22. Researched by a subagent,
> then AI-verified against the ported code (the load-bearing fact — the SAD matcher
> returns a `uint64` *distance*, not a `[0,1]` confidence — was confirmed in
> `modules/vision/source/vision/sad.hpp:41,59`). NOT committed.
>
> Scope: **S0 shared foundation** + **P0-A annotation workbench** per
> [`2026-07-21-product-form-and-roadmap.md`](2026-07-21-product-form-and-roadmap.md) §三
> (delivery slices S0 / A1 / A2 / A3) and the four 留待 design points. Resolves the
> four D3 留待 items (manifest/schema, ROI coordinate space, page-signature
> semantics, handle namespace) and the D8 coordinate seam — all **P0 = identity-only,
> the seam must hold non-identity later**.
>
> Authorities honored: D3, D8, DESIGN §5.3/§7/§8.6/§11, and the **HARD rule: page
> priority is diagnostic ordering only and NEVER suppresses `AmbiguousScreen`**.
> Soul constraints carried through every section: determinism, full traceability,
> strict-background, zero game-specific branches in core, assets separate from code,
> Luau sees only read-only opaque handles.
>
> **⚑ 9 open questions in §8 need the developer's decisions before A1/A2 coding — the
> sharpest are OQ-1 (threshold unit) and OQ-2 (Ambiguous/Unknown as a result type vs a
> new `AutomationErrorKind`).**

## 0. Grounding facts from the ported code (these constrain every decision)

Verified in-tree, not assumed:

| Fact | Location | Design consequence |
|---|---|---|
| Coordinate spaces `Desktop→Client→Frame→Normalized` already exist; `Point<Space>`/`Rect<Space>` are `float`, `PixelRect` is validated `uint32`. | `modules/domain/source/domain/space.hpp` | The annotation "base" space is **`FrameSpace` at `base_resolution`**. `NormalizedSpace` + `frameToNormalized`/`normalizedToFrame` are already the P1 seam — do not invent a new space. |
| `CoordinateTransform::create(clientOrigin, clientW, clientH, frameW, frameH)` encodes `{scale, offset, viewport}` implicitly. | `space.hpp` | D8's conceptual `{scale,offset,viewport}` maps 1:1 onto this class. P0 identity = client rect == frame extent == base_resolution. |
| SAD matcher returns `SadMatch{x,y,score}` where **`score` is sum-of-absolute-differences, `uint64`, lower=better, `0`=perfect** — a *distance*, not a `[0,1]` confidence. | `modules/vision/source/vision/sad.{hpp,cpp}` (`uint64 m_score`, `sad.hpp:41,59`) | `threshold` in the manifest is `[0,1]`; it must be compiled to an **integer max-SAD bound** (§1.4). The single most important correctness detail for runtime-identical Preview. |
| Grayscale is deterministic integer math: `(77·R + 150·G + 29·B) >> 8`. | `sad.cpp` `bgra8ToGray8` | Preview MUST call this exact function. No separate preview path. |
| ROI is a `PixelRect` (integer FrameSpace px); the matcher iterates integer candidates. | `sad.cpp` | Store ROI as **integer pixels**, not floats — see §2. |
| `Detection{sessionId,targetGeneration,frameId,label,Rect<FrameSpace>,confidence}` + `ObservationLease` already exist; `max_action_frame_age=750ms`. | `modules/domain/source/domain/detection.hpp` | Annotation recognizers produce exactly this `Detection`. `action_target`'s default click is an anchor only — the runtime still binds to the live `Detection` under lease. |
| WGC capture: `WgcCaptureSession::create(...).capture() -> Frame`; `ClientGeometry::transformFor(frameW,frameH) -> CoordinateTransform`. | `modules/controller/source/controller/capture.hpp` | The workbench reuses this verbatim for "grab current frame", getting a real `CoordinateTransform` for free. |
| `AutomationErrorKind` has **no** `AmbiguousScreen`/`UnknownScreen`. | `modules/domain/source/domain/error.hpp` | Page recognition returns a **`PageOutcome` result type**, not an error; it only becomes a fail-closed error when a script tries to *act* on it (§3, OQ-2). |

---

## 1. Data model / schema

### 1.1 File placement — recommendation: one GUI-owned, schema-versioned `annotations.toml`

| Option | Verdict |
|---|---|
| **A. Embedded in `project.toml`** | **Rejected.** `project.toml` is hand-authored (capability/target/task). The GUI rewrites recognizers/pages on every edit; mixing machine-churn into the hand-authored file invites clobbering + merge pain. Violates "assets separate from code". |
| **B. Two files** `recognizers.toml` + `pages.toml` | Acceptable but adds an inter-file closure (pages → recognizers) for no P0 benefit. |
| **C. One machine-owned `annotations.toml`** (both `[[recognizer]]` and `[[page]]`) | **RECOMMENDED.** One closure to validate; atomic one-click rewrite; clean ownership; `project.toml` stays hand-authored and references it. |

```toml
# project.toml (hand-authored; one added line vs DESIGN §11.2)
[project]
id = "personal.chaos-dreamscape"        # 卡厄斯梦境

[targets.windows]
baseline_client_size = [1920, 1080]     # project-wide base_resolution anchor (§2)
baseline_dpi_scale   = 1.0

[annotations]
path = "annotations.toml"               # GUI-owned; hand edits discouraged
```

TOML (not JSONC) for `annotations.toml`: it is machine-generated tabular data (arrays
of tables), round-trips cleanly and diffs readably; `project.toml`/`compatibility.toml`
are already TOML. (DESIGN §8.1 chose JSONC for *flows* because flows are hand-authored
control structures — annotations are neither.)

### 1.2 Schema versioning

```toml
schema = "umbraflow-annotations/v1"     # independent of flow/engine schema (DESIGN §3.7)
base_resolution = [1920, 1080]          # document default; per-recognizer may override (OQ-6)
base_dpi_scale  = 1.0
```

Unknown **major** → load-time reject (`InvalidResource`); newer **minor** → best-effort
load + a Warning trace event. The workbench writes the newest minor it knows. Templates
are content-hash addressed (§6), so template bytes version independently.

### 1.3 Recognizer entry

```toml
[[recognizer]]
name  = "home_marker"          # unique; becomes Detection.label and the Luau handle key (§4)
kind  = "template"
template = "assets/templates/home_marker.7f3a…c9.png"   # content-hash-addressed lossless PNG (§6)
roi   = [1180, 40, 220, 90]    # [x, y, w, h] INTEGER px in base_resolution FrameSpace (§2)
threshold = 0.90               # [0,1] confidence floor; compiled to an integer max-SAD bound at load (§1.4)
grayscale = true               # true → runtime & Preview both apply bgra8ToGray8
base_resolution = [1920, 1080]
# provenance (written by the asset pipeline §6; ignored by the matcher)
source_screenshot = "sha256:ab12…"
template_hash     = "sha256:7f3a…c9"

[[recognizer]]                 # color recognizer (info_region / lightweight state)
name  = "stamina_full_glow"
kind  = "color"
roi   = [90, 20, 40, 40]
space = "hsv"                  # rgb | hsv
low   = [40, 120, 120]
high  = [80, 255, 255]
min_ratio = 0.35               # min fraction of ROI pixels in range (DESIGN §7.2 ColorMatch)

[[recognizer]]                 # composite (declarable in schema; P0 MAY ship template+color only — §7)
name = "on_battle_result"
kind = "composite"
op   = "all"                   # all | any | not | count
members = ["result_banner", "not(loading_spinner)"]
```

### 1.4 Threshold → integer bound (the determinism-critical detail)

SAD is a `uint64` distance; a float `threshold` must not enter a *decision* (soul:
determinism). Resolution:

- **At load**, per template: `templatePixels = tw·th`; `maxSad = ⌊(1 − threshold) · 255 · templatePixels⌋` (floor, one fixed rounding).
- **At match** (runtime **and** Preview, same code): predicate is the **integer** compare `sadScore ≤ maxSad`. No float in the hit/miss decision.
- **`confidence` for display/trace only**: `1.0 − sadScore / (255.0 · templatePixels)` — never a decision input.

This is what makes Preview provably runtime-identical: same `bgra8ToGray8`, same
`matchTemplateSad`, same integer predicate. The workbench must not carry its own matcher.

### 1.5 The three annotation types

| Type | Purpose | Compiles to | Luau surface |
|---|---|---|---|
| `page_anchor` | Evidence a named page is / is not on screen | `[[recognizer]]` + `required`/`forbidden` entry in `[[page]]` | via `bot.pages.<page>` only |
| `action_target` | An interactable target the script may click | `[[recognizer]]` (+ `default_click` **anchor only**) | `bot.templates.<name>` → `frame:find` → `Detection`; click binds to the live `Detection` under lease, never to `default_click` |
| `info_region` | Text / number / icon / status readout | `[[recognizer]]` (template or color in P0; OCR only if a daily is blocked on text) | `bot.templates.<name>` (read-only detection) |

Each annotation rectangle carries `{ id (stable UUID), name, type, recognizer-params,
page-membership }`; the UUID survives renames so undo/redo + regression refs stay stable.

---

## 2. Coordinate space (the D8 open item)

**Recommendation: store absolute integer pixels in an explicit `base_resolution`;
normalized `[0,1]` is the derived P1 seam, not the stored source of truth.** (This
reverses an earlier casual "normalized+base" leaning — grounded now in the actual recognizer.)

| Criterion | Normalized `[0,1]` + base_res | **Absolute px in base_res (RECOMMENDED)** |
|---|---|---|
| Fidelity to the recognizer | SAD/ROI are integer px; normalized adds a round-back-to-px rule in the hot path; template size is px anyway | Manifest values are byte-identical to what `matchTemplateSad`/`PixelRect` consume — zero conversion |
| Determinism | Extra float→int rounding per load (must be pinned) | No rounding; integers throughout |
| Redundancy | Storing normalized **and** base_res is redundant | base_res is the sole anchor; px exact |
| Resolution independence | "free" on its face | but still needs the viewport (letterbox); which base_res + transform already give |

Source of truth = **`AnnotationSpace ≡ FrameSpace @ base_resolution`, integer px**.
`NormalizedSpace` + `frameToNormalized`/`normalizedToFrame` (already in `space.hpp`) stay
as the display representation and the P1 letterbox seam — derived on demand, never stored.

**Flow (one conceptual `baseToFrame` above the existing `Client↔Frame`):**
```
AnnotationSpace (base px) --baseToFrame--> FrameSpace (capture px) --frameToClient--> ClientSpace --> DesktopSpace
       stored ROI/template                    live capture           (existing CoordinateTransform)
```
- **P0 (identity):** `baseToFrame = identity`, enforced by a fail-closed gate at task start: runtime `frame_size == base_resolution` **and** `dpi_scale == base_dpi_scale`, else fail `TargetCompatibilityUnverified` — no silent scale, no silent match (D8).
- **P1 seam (deferred, pre-wired):** `baseToFrame = {scale=uniformScale, offset=letterboxOrigin, viewport=scaledBaseRect}` — exactly D8's triple and exactly what `CoordinateTransform::create` encodes. The template bitmap itself must be resampled by `uniformScale` in P1 (flag the sub-pixel/peak-degradation caveat now). P0 does zero resample.
- **DPI:** record `dpi_scale` per document; treat logical/physical mismatch as a first-class compatibility fingerprint. P0 requires equality; P1 folds `dpi_scale` into `uniformScale`.

Because ROI is already integer FrameSpace px and the code has `frameRectToPixelRect` +
`normalizedToFrame`, adding `baseToFrame` in P1 is *filling in non-identity values*, not
a rearchitecture. Trace records the actual `scale/offset/viewport` per run → P1 stays replayable.

---

## 3. Page-signature semantics

A page is a named signature, not "one small image matched".

```toml
[[page]]
name     = "home"
priority = 10                  # DIAGNOSTIC ORDERING ONLY (§3.4) — never suppresses Ambiguous
required = [ { recognizer = "home_marker" },
            { recognizer = "home_menu_bar", threshold_override = 0.85 } ]   # ALL must match
forbidden = [ { recognizer = "battle_hud" } ]                              # NONE may match
```

**Match rule (per frame):** a page is a **candidate** iff every `required` hits
(`sadScore ≤ maxSad`) **and** no `forbidden` hits. Evaluate all pages → candidate set `C`:

| `|C|` | `PageOutcome` |
|---|---|
| 1 | `Resolved(pageId, evidence)` |
| 0 | `Unknown(evidence)` |
| ≥ 2 | **`Ambiguous(pageIds, evidence)`** |

**Threshold overrides:** a page-membership entry MAY carry `threshold_override` (changes
the integer `maxSad` for that page's use of that recognizer only; recomputed at load;
`[0,1]` or reject; only on scalar/template members).

**§3.4 Page priority = diagnostic ordering ONLY (the HARD rule).** `priority` orders the
evidence list + diagnostic display; it **NEVER** collapses `|C| ≥ 2` into a resolved page.
Allowing it would silently convert fail-closed "multi-hit → AmbiguousScreen → do not click"
into a click, puncturing "never click the wrong screen".

> **Regression test:** pages `home` (pri 10) and `home_with_banner` (pri 5) both become
> candidates on one frame. Priority reorders the evidence so `home` is first, **but the
> outcome is `Ambiguous([home, home_with_banner])`, not `Resolved(home)`.** The only fix is
> authoring a distinguishing `forbidden` anchor, not a priority tweak.

**§3.5 Unknown/Ambiguous evidence format** (structured, emitted to trace on the spot):
```jsonc
{ "outcome": "ambiguous", "candidates": ["home","home_with_banner"],
  "frame_id": 4213, "target_generation": 7,
  "pages": [ { "page":"home","priority":10,"candidate":true,
      "required":[ {"recognizer":"home_marker","hit":true,"sad":210300,"max_sad":244800,"confidence":0.972} ],
      "forbidden":[ {"recognizer":"battle_hud","hit":false,"sad":5120000,"max_sad":244800,"confidence":0.310} ] },
    { "page":"home_with_banner","priority":5,"candidate":true, "required":[…], "forbidden":[…] } ] }
```
Every anchor reports `hit`, integer `sad`, integer `max_sad`, display `confidence` — enough
to replay offline + drive the Preview panel colouring. `Unknown` uses the same shape with
`candidate:false` on all pages (author sees *how far* each missed).

**§3.6 Load-time validation** (all statically decidable; before any VM): template path
resolves + hash matches; every page recognizer name resolves (reference closure);
`required ∩ forbidden = ∅`; ROI ⊆ base_res frame and template `(tw,th) ≤ (roi.w,roi.h)`;
thresholds ∈ `[0,1]`, color `low ≤ high`, composite members resolve + acyclic;
`threshold_override` only on scalar members; names unique — all → `InvalidResource`.
**Static page ambiguity** (two pages with identical `required` and no distinguishing
`forbidden`) → **Warning** by default, `--strict` → error (OQ-3).

---

## 4. Luau handle namespace

The host injects, per run, a **recursively read-only** table populated **100% from
`annotations.toml`** at VM creation:
```lua
local frame = bot:capture()
local d = frame:find(bot.templates.home_marker)   -- opaque recognizer handle → Detection | nil (Tier A)
if bot.pages.home:matches(frame) then … end        -- opaque page handle → PageOutcome
```
- `bot.templates.<name>` / `bot.recognizers.<name>` — one opaque handle per declared
  recognizer; `bot.pages.<name>` — one per page.
- Handles are **opaque**: no `path`/`roi`/pixel field readable; **no** `template(path,roi)`
  constructor; **no** `frame:find("home.png")` string path. Assets are declared by the GUI,
  period (D3). This is the "store locator, not element" discipline.
- Layer3 parameterized ROI (roi computed from another detection) is **C++-side, P0-deferred**;
  Lua never does coordinate arithmetic.

**Literal-only + the load-time 100% enumeration check (three layers, honest about Lua's undecidability):**
1. **Manifest closure enumeration (authoritative, 100%, load-time):** enumerate every declared
   recognizer + page, verify assets/entries (§3.6), build the readonly handle tables to contain
   exactly that closed set. Complete because it is over the *finite static manifest*.
2. **Literal-reference lint (best-effort, load-time):** reject dynamic handle access in P0
   (`bot.templates[expr]`, computed field) → handle refs are literal. Lint, not proof (imperative
   Luau is undecidable); reject-on-doubt.
3. **Runtime nil backstop (fail-closed):** a field miss → `nil`; consuming a `nil` handle raises a
   structured error, never a silent no-op.

This tri-layer is the C++ answer to the "resource references statically enumerable" property that
imperative scripting otherwise loses: enumeration moves to the *manifest*; the script side degrades
to lint + fail-closed backstop.

---

## 5. Annotation GUI workbench (P0-A)

**Tech stack — Dear ImGui + D3D11, reusing WGC capture + the one-and-only vision matcher.**

| Layer | Reuse | Why |
|---|---|---|
| UI | **Dear ImGui** (docking) on a **D3D11** swapchain | Immediate-mode fits a solo tool; the capture path is already D3D11 (`capture-d3d.hpp`) so a captured `Frame` texture uploads to an ImGui image cleanly. Standalone `.exe`, later hostable by the P2 tray app. |
| Capture | **`WgcCaptureSession`** verbatim | "Grab current WGC frame" = the exact runtime capture, giving the canvas a real `CoordinateTransform`. |
| Recognition/Preview | **`matchTemplateSad` + `bgra8ToGray8` + the §1.4 integer predicate**, unmodified | The "runtime-identical" guarantee — Preview literally links the runtime matcher; no parallel path can drift. |
| Import | PNG decode (lossless) → BGRA → same pipeline | Imported screenshots and live frames are interchangeable inputs. |

**Minimal user loop (nothing deferrable to "UI later" — roadmap §三.93/105):**
1. **Source** — target window → grab WGC frame, or import PNGs. Left rail = sample list;
   selecting one swaps the canvas. Each source stores original client size, DPI, target
   generation, capture backend, timestamp, content hash; the screenshot is never mutated.
2. **Canvas** — zoom/pan, box-select/move/resize/copy/delete, **undo/redo**. Coordinates always
   shown in explicit `base_resolution` FrameSpace (overlay hit-testing may use `frameToNormalized`;
   stored values stay px).
3. **Property panel** — edit name, type, owning page, required/forbidden role, kind, threshold,
   grayscale; `action_target` may set `default_click` but **cannot bypass the runtime lease**.
4. **One-click generate** — crop template(s), write/update `annotations.toml` + page signatures
   atomically, run §3.6 validation, surface failures inline. **Author never hand-edits config.**
5. **Preview/Test** — run the runtime recognizer on the current image or all samples; show
   hit-boxes, `confidence`, expected-vs-actual page, Unknown/Ambiguous reasons; one click files a
   result into the positive / negative / confusable regression set.

**Document model:** an immutable value (`{sources[], recognizers[], pages[]}` keyed by stable
UUIDs); edits are commands (`AddBox`, `ResizeBox`, `Rename`, `SetThreshold`, `SetPageRole`…) on an
undo stack; Preview results are transient (never in the document, so undo can't resurrect stale
confidences). "Generate manifest" is a pure function of the document → golden-testable.

**Reuse crux:** exactly one matcher in the process. Preview calls `bgra8ToGray8` → `matchTemplateSad`
→ `sadScore ≤ maxSad` — the same three the runtime makes; the workbench adds *rendering*, never
*recomputation*. (Mirrors ok-script's bounding-box debug overlay shape while keeping UmbraFlow's own
format + safety contract.)

---

## 6. Asset pipeline

1. **Crop** the annotation ROI at `base_resolution` px from the source BGRA buffer; store as
   **lossless PNG** (DESIGN §10.3 — lossy would break integer-SAD determinism).
2. **Content-hash** `sha256` over the PNG bytes; the template is addressed by hash (DESIGN §7.4/§11.1).
   Filename may embed a hash prefix; the hash is the identity.
3. **Provenance** on the recognizer: `template_hash`, `base_resolution`, `base_dpi_scale`,
   `source_screenshot` hash — every template traceable to the exact capture + geometry (soul: traceability).
4. **Grayscale derived, not stored:** keep the color PNG as the single source of truth; `grayscale=true`
   applies `bgra8ToGray8` at load (deterministic, lossless decode).

**Feeding validation:**
- **Static regression set:** positive / negative / confusable = stored content-hashed screenshots + an
  *expected* `PageOutcome` each; the runner replays through the one matcher and diffs actual vs expected;
  the diff never overwrites expected (Golden Image discipline). Expected `Unknown`/`Ambiguous` must stay so.
- **Fake Controller frame sequences:** the same screenshots sequenced into a scripted `capture()` stream so
  the B-side `observe/act/wait` loop runs end-to-end offline, deterministic + replayable. Assets carry
  `base_resolution`, so the P0 identity gate is exercised in-sequence.

---

## 7. Scope boundary — what P0-A explicitly does NOT do

No task-graph editor / script recording / asset marketplace / multi-user / project management /
polished HTML reports (P1/P2). No Layer3 parameterized ROI (C++-side, deferred; Lua never does coord
math). No resolution adaptation (`baseToFrame` identity-only; mismatch fail-closed, not scaled; uniform
scale is P1). No OCR unless a real daily is provably blocked on text (`info_region` uses template/color
first; neural OCR out on determinism grounds). No dynamic handle indexing in Lua (literal only). Composite
recognizer is *declarable* but P0 MAY ship template+color and defer Composite (OQ-8). No `.umbraflowpack` /
signing / `manifest.lock` (a project is a local dir). The workbench does not inject input and is not a
runtime overlay (that overlay, with `WS_EX_NOACTIVATE`/`WDA_EXCLUDEFROMCAPTURE`, is P2). **Nothing in the
§5 minimal loop may be deferred with "UI later".**

---

## 8. Open questions the developer must decide (each affects P0-A code)

1. **OQ-1 — Confidence mapping sign-off.** §1.4 proposes `maxSad = ⌊(1−threshold)·255·pixels⌋` with an
   integer decision predicate. Alternative: express the bound directly as `max_sad_per_pixel` and drop the
   `[0,1]` abstraction (more transparent, less familiar). Authored unit = `threshold∈[0,1]` (recommended,
   ok-script-familiar) or raw per-pixel SAD?
2. **OQ-2 — Are `AmbiguousScreen`/`UnknownScreen` `AutomationErrorKind` variants, or a `PageOutcome` result
   type?** Recommendation: a `PageOutcome` value from page recognition (normal control flow), promoted to a
   **new fail-closed error kind only when a script acts** on an unresolved screen. Needs `domain/error.hpp`
   additions either way — confirm two new kinds (`AmbiguousScreen`, `UnknownScreen`) or one
   `ScreenNotResolved{ambiguous|unknown}`.
3. **OQ-3 — Static page-ambiguity: warning or error?** Recommendation: Warning by default, `--strict` →
   error; confirm the default.
4. **OQ-4 — `info_region` default recognizer** (template vs color) and the concrete trigger to pull OCR
   forward (which 卡厄斯梦境 daily, if any, reads a number).
5. **OQ-5 — Where do source screenshots live, and do they ship?** Recommendation: regression-set screenshots
   ship (golden inputs); raw exploratory captures do not.
6. **OQ-6 — `base_resolution` granularity** — per-document only, or per-recognizer override allowed?
   Recommendation: allow in schema, warn in UI if they differ from the document default.
7. **OQ-7 — Handle namespace spelling** — `bot.templates` + `bot.pages` vs a single `bot.recognizers`.
   Recommendation: expose both `bot.templates` (alias) and `bot.recognizers` (general) + `bot.pages`.
8. **OQ-8 — Composite in P0 or fast-follow?** Schema reserves `kind = composite`; does P0-A *implement*
   All/Any/Not/Count now or ship template+color and defer? (Deferring is cheap; implementing lets
   `on_battle_result`-style pages be authored day one.)
9. **OQ-9 — Undo/redo & Preview coupling** — confirm Preview results are transient and "generate manifest"
   is a pure function of the document (enables a golden test on manifest output).

---

### References (reference-only; ok-script is AGPL — UX/data-model shape learned, no code copied)
- ok-script framework & annotation/COCO/debug-overlay shape: https://github.com/ok-oldking/ok-script
- COCO annotation tooling shape: https://github.com/jsbroks/coco-annotator
- Multi-scale template matching + sub-pixel caveats (P1): https://pyimagesearch.com/2015/01/26/multi-scale-template-matching-using-python-opencv/
- Letterbox / viewport-relative coords (P1 seam): https://gamedev.net/tutorials/_/technical/apis-and-tools/stretching-your-game-to-fit-the-screen-without-letterboxing-sdl2-r3547/
- Windows DPI logical/physical pitfall: https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-screenscaling
- "Store locator, not element" (opaque-handle discipline): https://www.browserstack.com/guide/stale-element-reference-exception-selenium , https://playwright.dev/docs/actionability
