# Page-centric authoring: domain model v2 and the editing layer

> **Redirect (2026-07-31).** This plan landed, and the model it produced has
> since been changed by
> [`2026-07-31-annotation-model-capabilities.md`](2026-07-31-annotation-model-capabilities.md).
> Read the rulings below as history, with three specific reversals:
>
> 1. §2 item 5 ruled that the authoring-only `shared` flag **stays what it is
>    today**, carried on the element. That is **reversed**: the flag is deleted
>    and replaced by `Holding{Owned, Referenced}` on the page reference. The
>    capability plan §2.2 owns the reasoning (a flag written once by
>    `shareRegionOnPage` could contradict the placements with nothing noticing).
> 2. §2 item 3's "one element, N placements" is exactly right and survives; the
>    row is now called `PageReference` and carries `holding` and `exercised`
>    besides `{pageId, elementId, searchRoi}`.
> 3. The **runtime** contract this plan promised would stay frozen throughout
>    (`k_runtimeManifestSchema`, `annotations.runtime.toml`,
>    `RecognitionRuntime`) was un-frozen on 2026-07-31. Both schemas were bumped
>    in one atomic change: `umbraflow-authoring/v2` → `/v3` and
>    `umbraflow-annotations/v1` → `/v2`. The `PERMANENT BRIDGE` comment this plan
>    left in `authoring-document.cpp` went with it.
>
> One more mechanism this plan introduced is gone:
> `derivedRuntimeRecognizerId(elementId, pageId)` and the per-placement expansion
> around it. The compiler now emits exactly **one** recognizer per element under
> the element's own id, because a page's refinements are read from the reference
> row at match time instead of being baked into a separate recognizer per page.
>
> The GUI this plan's editing layer was built for was archived in `b57b67b`;
> the editing layer itself remains and is driven by `umbra-authoring`.

Status: proposed; amended 2026-07-26 after adversarial review. Supersedes
one clause of the redesign agreed in the 2026-07-25 session notes: that
agreement said "no schema, runtime, or S0 contract change is planned"; the
user has since decided the **authoring** schema may change where the
requirements demand it. The **runtime** contract stays frozen throughout:
`k_runtimeManifestSchema`, the generated `annotations.runtime.toml`, and
`RecognitionRuntime` do not change in any phase of this plan.

**S0 amendment.** Phase 2 knowingly overrides three clauses of the LOCKED
S0 block in `2026-07-22-annotation-design.md` §1.2: "exact schema
identifier `umbraflow-authoring/v1`", "minor-version best-effort loading
is deferred", and "reopening rejects schema drift". The v1-to-v2 loader
migration is precisely the deferred capability, now un-deferred by user
decision (2026-07-26). The S0 document receives a matching amendment note
when phase 2 lands; every other S0 clause — the runtime manifest schema
above all — remains in force.

## 1. Problem

The author thinks in pages: "this page is identified by these marks, and
has these buttons on it." Neither the storage model nor the editing code
speaks that language.

Storage. `Catalog` is three flat vectors joined by ids. The page-to-region
edge is stored inverted — `RecognizerDefinition::m_allowedPageIds` on the
recognizer — so "what is on this page" exists nowhere and every consumer
re-derives it by scanning all recognizers (`panels.cpp` does this join in
three places). `RecognizerDefinition` is one product type carrying three
variants, held together by cross-field rules enforced at `create()`:

- only `action_target` may define a default click;
- `page_anchor` must have empty `allowed_page_ids`;
- `action_target` must have non-empty `allowed_page_ids`.

These rules are why `retypeRecognizer` exists: a per-widget editor cannot
cross between the variants, so retyping must rewrite four interlocking
fields as one repair transaction.

Editing. The operations live as free functions over `(AppState&,
PanelUiState&)` in `authoring-actions.cpp`. They are tested and correct,
but nothing groups "everything you can do to a page" or "everything you
can do to a region", and the panels assemble each page by hand every
frame.

The shared-region feature works today by **copying**: `shareRegionOnPage`
mints a new recognizer per page, each with its own search ROI, so per-page
ROI is already delivered. What that costs is one element pretending to be
N: the copies are grouped only by coincident template bytes
(`sharedRegionMembers`), and a template correction must be manually
carried across every member (`retemplateSharedRegion`). The model has no
way to say "one element, placed on N pages" — it can only say "N elements
that happen to look identical", and every consumer must remember they are
secretly one thing.

## 2. Target domain model (authoring schema v2)

Two kinds of thing, with every page-membership edge on the page.

```
Element                                # project-owned, template authored once
  id, name
  template   = sourceId + templateRect # the pixels, cut from one screen
  threshold
  kind: one of
    Anchor        {}                   # identity evidence
    Interactive   { clickOffset? }     # something the runtime may click
    Info          {}                   # readable region, no runtime consumer yet

Page                                   # the aggregate the author thinks in
  id, name
  required:  [elementId]               # anchors that identify this page
  forbidden: [elementId]               # anchors that must not match here
  placements: [ { elementId, searchRoi } ]
                                       # interactive/info elements on this
                                       # page, and where to look for each
                                       # of them HERE

Regression                             # unchanged: screen -> page claim
```

Consequences, in order of importance:

1. **`allowed_page_ids` disappears from authoring.** Placement is a
   page-side fact. The compiler derives the runtime `allowed_page_ids` by
   inverting placements at generation time — one inversion, in one place,
   instead of one join per UI panel. The runtime manifest keeps its
   current schema string and shape.
2. **Two of the three cross-field rules die structurally.** A default
   click exists only inside `Interactive`; anchors have no placement
   field to misuse. The third rule survives as a document-closure rule
   where it belongs: every `Interactive` element must appear in at least
   one placement.
3. **Sharing becomes what it claims to be: one element, N placements.**
   The per-page copies, the coincident-template grouping, and the
   member-carrying retemplate machinery all disappear — a template edit
   touches one element and every placement sees it. Per-page ROI, already
   delivered by the copies today, carries over natively on the placement.
   Anchors keep an element-level search ROI: their page membership is the
   signature, and no current requirement needs a per-signature ROI.
4. **Retype shrinks.** Changing an element's kind is constructing a
   different variant plus adjusting placements; the repairs that invented
   or stripped `allowed_page_ids` vanish. It remains a transaction.
5. The authoring-only `shared` flag stays what it is today — author
   intent that an element is offered for reuse — carried on the element.

C++ shape: `Element` holds a `std::variant` of the three kind payloads,
matched with the existing `core/utility/variant-match.hpp`. Values
throughout; no hierarchy, no virtuals, in keeping with the module's style.

## 3. The editing layer (workbench support target)

Storage stays canonical and validated in `modules/annotation`; the
workbench gains an object per user-facing concept. All of it is
ImGui-free and lands in the support target, reachable from
`test-workbench`.

### Ownership ledger

The whole design reduces to five ownership statements:

| Object | Owns | Borrows |
|---|---|---|
| `AuthoringEditHistory` (exists) | every committed version, plus a monotonic **revision counter** | nothing |
| `EditPage` | a full `AuthoringDraft` **copy**, the target `PageId`, and the base revision stamped at `open()` | nothing |
| `PageAnchor` / `InteractiveRegion` handles | nothing | their `EditPage`, local-scope only: **non-copyable and non-movable**, so storing one does not compile |
| `PageView` (drawing snapshot) | its own values — authored data and ids **only**, no margins | nothing |
| `PendingEdit` queue (exists) | the draft in transit to this frame's commit | nothing |

Two guards added by the 2026-07-26 review, replacing convention with
mechanism:

- **Stale commits are refused.** `EditPage` records the history revision
  it was opened against; `applyEdit` rejects a draft whose base revision
  is no longer current. An `EditPage` accidentally stored across frames
  can no longer silently resurrect state the author undid — the commit
  fails with a visible error instead.
- **Handles cannot escape.** They are non-copyable *and* non-movable, so
  they exist only as locals inside a draw; a `PanelUiState` member of
  handle type does not compile.

A draft carries no pixel data (`EditableSource` is id + hash + fingerprint
+ provenance), so the copy `EditPage` takes is microseconds. That price
buys the load-bearing property: **zero stored borrows into the live
document.** The dangling-borrow class of bug that `drawPropertiesPanel`
had cannot recur in this layer, because nothing in it points into
something `applyEdit` replaces.

### EditPage

```cpp
// One page, opened for editing. Owns a copy of the whole project draft --
// the draft is the unit of rebuild-and-validate, so there is no such
// thing as an edited half-page. Every operation mutates the owned copy;
// the live document is untouched until commit.
//
// Frame-scoped: open, edit, and commit within one draw. This is enforced,
// not asked for -- the editor records the history revision it was opened
// against, and a commit whose base is no longer current is refused. An
// EditPage stored across frames cannot resurrect state the author undid;
// its commit fails visibly instead.
class EditPage final
{
    AuthoringDraft     m_draft;
    annotation::PageId m_id;
    uint64             m_baseRevision;

public:
    [[nodiscard]] static auto open(AppState const&, annotation::PageId) -> Result<EditPage>;
    [[nodiscard]] static auto createFrom(AppState const&, annotation::SourceId) -> Result<EditPage>;

    // Queries answer from the owned draft; the panels draw from these.
    [[nodiscard]] auto view() const -> PageView;

    // Signature (anchor) edits. placeAnchor mints a new identifying mark
    // against a screen, the "+ Identifying mark" button's home.
    [[nodiscard]] auto placeAnchor(NewAnchorSpec) -> Result<AddedAnchor>;
    auto requireAnchor(MemberId) -> Status;
    auto forbidAnchor(MemberId) -> Status;
    auto claimScreen(annotation::SourceId) -> Status;
    auto classifyScreen(annotation::SourceId, RegressionClassification) -> Status;

    // Region membership. placeRegion mints; placeExisting authorizes an
    // element that already exists (the manual checkbox path); accept is
    // the shared-palette drop.
    [[nodiscard]] auto placeRegion(NewRegionSpec) -> Result<AddedRegion>;
    auto placeExisting(MemberId) -> Status;
    [[nodiscard]] auto acceptSharedRegion(MemberId from) -> Result<SharedRegionScore>;

    // Handles onto members of this page: local-scope only (non-copyable,
    // non-movable), resolved by id on every call.
    [[nodiscard]] auto anchor(MemberId) UF_LIFETIME_BOUND -> Result<PageAnchor>;
    [[nodiscard]] auto region(MemberId) UF_LIFETIME_BOUND -> Result<InteractiveRegion>;

    // The only exits. Both route through the existing requestEdit queue,
    // preserving the one-commit-per-frame guard and, for the selecting
    // form, select-only-after-landing. Consuming the editor is what makes
    // "one EditPage, one commit" structural.
    auto commit(PanelUiState& ui, std::string description) && -> void;
    auto commitSelecting(
        PanelUiState& ui,
        std::string description,
        MemberId select,
        std::optional<annotation::SourceId> selectSource
    ) && -> void;
};
```

`MemberId` is the page-local key. In phase 1 it **is** `ElementId` —
the v1 model has no element identity, and a shared region on page P is
its own recognizer there, so keying by recognizer is what makes
`setSearchRoi` unambiguous over v1. In phase 3 it becomes the placement
key `(ElementId on this page)`; the API shape does not change.

### InteractiveRegion / PageAnchor

Handles, not values: each holds `EditPage& + MemberId` and resolves by id
on every call, so a structural edit through one handle cannot dangle
another. **Non-copyable and non-movable**: a handle exists only as a
local inside the draw that asked for it — storing one in `PanelUiState`
does not compile. That mechanism, plus the revision guard on commit, is
what makes this borrow acceptable under the repository's stored-borrow
rule; the review considered id-keyed mutators on `EditPage` instead and
the user chose the handle form deliberately (operations live on the
object the author is thinking about).

```cpp
class InteractiveRegion final
{
    EditPage& m_page;   // local-scope borrow; the type cannot escape
    MemberId  m_id;

public:
    InteractiveRegion(InteractiveRegion const&)            = delete;
    InteractiveRegion(InteractiveRegion&&)                 = delete;
    auto operator=(InteractiveRegion const&) -> InteractiveRegion& = delete;
    auto operator=(InteractiveRegion&&) -> InteractiveRegion&      = delete;

    // Data (values, from the draft).
    [[nodiscard]] auto name() const -> std::string;
    [[nodiscard]] auto templateRect() const -> PixelRect;
    [[nodiscard]] auto searchRoiOnThisPage() const -> PixelRect;
    [[nodiscard]] auto threshold() const -> uint32;
    [[nodiscard]] auto clickOffset() const -> std::optional<TemplateOffset>;
    [[nodiscard]] auto isShared() const -> bool;
    [[nodiscard]] auto pagesPlacedOn() const -> std::vector<annotation::PageId>;

    // Operations (mutate the owning EditPage's draft).
    auto rename(std::string) -> Status;
    auto setThreshold(uint32 basisPoints) -> Status;
    auto setClickOffset(std::optional<TemplateOffset>) -> Status;
    auto setSearchRoi(PixelRect) -> Status;     // this page's placement
    auto setTemplateRect(PixelRect) -> Status;  // carries every shared
                                                // member, as retemplate
                                                // does today
    auto setShared(bool) -> Status;
    auto shareToPage(annotation::PageId) -> Result<SharedRegionScore>;
    auto removeFromThisPage() -> Status;        // withdraw the placement
    auto deleteEverywhere() -> Result<DeletedEntity>;
};
```

`PageAnchor` is the symmetric handle for signature members (rename,
rects including the shared-member-carrying template edit, threshold,
require/forbid toggles, delete).

### PageView: the reflective drawing surface

A per-frame value snapshot the panels iterate — the "draw the object"
form. It carries **authored data and ids only**:

```cpp
struct PageView final
{
    annotation::PageId    m_id;
    std::string           m_name;
    std::optional<annotation::SourceId> m_claimedScreen;

    std::vector<AnchorRow> m_identifiedBy;   // name, rects, shared marker
    std::vector<AnchorRow> m_mustNotShow;
    std::vector<RegionRow> m_regions;        // name, roi HERE, click offset
};
```

Margins, live scores, and screen verdicts are **not** embedded. They are
owned once, by `AppState`'s last `ModelCheck`, and the panels keep merging
them at draw time through the existing id-keyed lookups (`findMargin`,
`findScreenCheck`) — embedding them would create a second owner of "the
margin for X" and force a view rebuild whenever a check completes.
Drawing consumes views plus lookups; mutation goes through handles; the
two never mix.

### What deliberately stays outside these classes

Source import, undo/redo, save/publication, model-check start/collect,
and the pending-edit apply are project- or frame-scoped, not page-scoped.
Forcing them into `EditPage` would recreate the god-object this design
avoids. Also deliberately outside, per the 2026-07-26 review:

- **Retype.** It changes an element's kind, so a `region()` handle would
  outlive its own meaning mid-call. It stays a free catalog-level
  transaction; it is not a handle method.
- **The shared-regions palette.** It enumerates every shared element
  across the whole catalog — a project-level query no page-scoped object
  can produce. It stays a free query; only the *drop*
  (`acceptSharedRegion`) belongs to `EditPage`.
- **Fallback queries.** `pageName`'s short-id fallback for an absent page
  and `sourceOfRecognizer` over unassigned recognizers must answer where
  `EditPage::open` would rightly fail; both stay free functions.

## 4. Phasing

Each phase lands green through the full gate on its own; the runtime
manifest and `RecognitionRuntime` are untouched in all three.

**Phase 1 — editing layer over the v1 model (workbench only).**
Introduce `EditPage`, the two handles, and `PageView`, implemented on the
current storage (the `allowed_page_ids` joins move inside `EditPage` and
die nowhere else). `AuthoringEditHistory` gains the revision counter and
`applyEdit` the stale-base refusal. Panels are rewritten to draw views
and call handles. `MemberId` is `ElementId` in this phase, so
`setSearchRoi` writes that page-member's own ROI — already per-page
correct under the copy model; there is no ROI gap to document. Existing
`authoring-actions` functions become the implementation guts or fold in;
the queue tests (`test-authoring-actions.cpp`) migrate **unchanged**,
which is the check that `commit`/`commitSelecting` really route through
`requestEdit` rather than around it.

**Phase 2 — authoring schema v2 (`modules/annotation`).**
`Element` variant + `Page.placements`; bump `k_authoringDocumentSchema`;
the compiler derives runtime `allowed_page_ids` from placements. Execution
decisions fixed by the 2026-07-26 review:

- *TOML encoding.* `CanonicalTomlReader` has no inline-table or nested
  table support, so placements serialize as a flat top-level
  `[[placement]]` table (`page_id`, `element_id`, `search_roi`), with a
  new rank in the section-ordering guard. Canonical order: by `page_id`,
  then `element_id`, mirroring the existing id sorts.
- *The cross-field rules are NOT deleted from `catalog.cpp`.*
  `RecognizerDefinition::create` also validates the frozen runtime
  manifest parse path, so its three rules stay exactly where they are.
  The v2 `Element` is a new type that never passes through
  `RecognizerDefinition`; the rules simply have no field to fire on
  there. Deleting them from `catalog.cpp` would silently de-fang runtime
  manifest validation.
- *Migration.* A frozen v1 read-only parse path is retained; it feeds the
  in-memory derivation (placements from `allowed_page_ids`) and
  **bypasses the canonical round-trip self-check**, documented at the
  bypass site — v1 files are read once and upgraded, so no v1 serializer
  is kept. Saves write v2 only.
- *New validations the placements model makes necessary.* A placement may
  reference only an `Interactive` or `Info` element, never an `Anchor`;
  no element may be both a required/forbidden anchor and a placement on
  the same page; every `Interactive` element must appear in at least one
  placement (the closure rule).

Retype loses its membership repairs. This phase touches serialization,
the compiler, and the annotation test suites — wide, and *not* purely
mechanical: the fixtures that build drafts with `m_allowedPageIds`
directly (`test-authoring-edit.cpp`) are reshaped to placements.

**Phase 3 — native internals and cleanup.**
Swap `EditPage` internals to the v2 model, delete the join code and the
`Editable* allowed_page_ids` plumbing, enable real per-page ROI, record
the migration note in the knowledge docs.

Phase 1 alone delivers the user-facing goal (operations live on the
objects, UI draws pages reflectively). Phases 2–3 make the model tell the
truth underneath it and unblock per-page ROI. If phase 2 is ever
abandoned, phase 1 still stands.

## 5. Risks and costs

- **Canonical bytes churn once.** Document equality and undo dedup use
  canonical serialization; v2 changes the canonical form, so every saved
  project rewrites once on first v2 save. Content-addressed assets are
  unaffected (hashes cover PNG bytes, not the TOML).
- **Real projects.** Few exist; the in-memory loader migration covers
  them, and `test-real-regression` re-verifies after phase 2.
- **Test surface of phase 2** is the widest: authoring round-trip,
  compiler, and workbench suites all touch the recognizer shape.
- **The handle borrow** is the single lifetime contract, and it is backed
  by mechanism rather than convention: handles are non-copyable and
  non-movable (storage does not compile), and a stale `EditPage` commit is
  refused by the revision guard. Review any future change that relaxes
  either property.
- An anchor shared by two pages keeps one search ROI (element-level).
  Accepted asymmetry; revisit only on a real need.

## 6. Test plan

- Phase 1: `EditPage` open/commit/refuse cycles, handle operations
  (including `placeAnchor`, `placeExisting`, `classifyScreen`, and the
  shared-member-carrying `setTemplateRect`), the revision guard (a commit
  over a stale base is refused; undo between open and commit triggers
  it), and view assembly — all through `test-workbench` with the existing
  fixture style; the current `test-authoring-actions` queue and selection
  cases migrate unchanged.
- Phase 2: v1→v2 loader migration round-trip (v1 read path bypassing the
  canonical self-check, exercised explicitly); derived runtime manifest
  byte-identical for an equivalent model (the A1+B1 regression evidence);
  closure-rule rejections; the new placement validations (anchor never
  placed, placement targets Interactive/Info only, no element both
  required and placed on one page); canonical placement ordering
  round-trip.
- Phase 3: per-page ROI round-trip; a shared region searched at different
  rectangles on two pages, through the compiler and a synthetic runtime
  evaluation.

## 7. 2026-07-26 — per-page search ROI at generation time

Real-machine GUI verification of phase 3 surfaced two defects, both fixed
now.

**Compiler, not deriveModel.** The v2 truth is elements plus per-page
placements; the derived `catalog()` is the UI's read model and stays
one-recognizer-per-element (its `PERMANENT BRIDGE`). Expanding an element
into one recognizer per placement *there* would surface synthetic per-page
recognizers as phantom rows in every panel that reads `catalog()`. So the
expansion lives in `compileAuthoringDocument`, built from `elements()` and
`placements()` at generation time — the roadmap's own phrasing, "per-page
search ROI at generation time":

- anchors and unplaced elements → one recognizer, element id/name/ROI,
  exactly as the derived one;
- exactly one placement → one recognizer keeping the element id and name,
  `allowed_page_ids = [that page]`, and the *placement's* ROI. Migrated
  data has placement ROI == element ROI, so the golden manifest is
  byte-identical (verified — the golden test is unchanged);
- N ≥ 2 placements → N recognizers, one per placement in canonical order,
  each with that placement's ROI and single allowed page.

Templates still dedupe by `(source, template rect)`, so the N recognizers
of one element share a single template asset.

**Deterministic id/name scheme.** A per-placement recognizer needs an
identity distinct from the element's, stable across compiles. The id is
`sha256(elementId.toString() bytes ++ pageId.toString() bytes)` truncated
to 16 bytes through `ResourceId::fromBytes` — deterministic, and
collision-resistant enough that a clash with a real id is astronomically
unlikely and would fail loudly in `RecognitionCatalog::create`'s
uniqueness guard rather than silently drop a recognizer. The name is
`"<elementName>_<pageName>"`: both are already valid ASCII Luau member
keys so their underscore join is one too, it can never spell a reserved
word, and the page name is unique among pages so the pair reads as "which
element, on which page". Any residual name clash across elements is caught
loudly by the same guard.

**Canvas edits the placement, not the element.** The canvas resolves a
page context in the workbench layer (`placementContext`): the page that
claims the shown screen, narrowed to an interactive region that page
places. With a context the canvas draws and edits that page's placement
ROI (routed through `EditPage::open` + `region(id).setSearchRoi`, whose
handle was fixed to write the placement rather than the element field);
without one — an unclaimed screen, an anchor, or an unplaced region — the
edit writes the element's own default range and says so.

## 2026-07-26 — Recording negative and ambiguous regression cases

Until now every regression case the workbench could create was a
`ResolvedRegression`: `createPageFromSource` and `claimScreenForPage` both
pair `RegressionClassification::Positive` with "resolves to this page", and
the Properties panel's Regression section could only *relabel* the
classification enum of a case that already existed — it early-returned when
the shown screen had none. `UnknownRegression` and `AmbiguousRegression`
were never constructed anywhere under `entry/workbench`, so a real suite
could not record "this screen is none of my pages" or "this screen is
allowed to be ambiguous". That is the gap this closes.

**What "negative" means here.** The runner checks the *expectation* variant
(`matchesExpectation`): `Resolved{page}` must resolve to that page,
`Unknown` must resolve to no page, `Ambiguous` must come back as
`AmbiguousPages`. The `RegressionClassification` enum
(`Positive`/`Negative`/`Confusable`) is the descriptive label paired with
it. A negative case is therefore a screen recorded as `UnknownRegression` —
it must resolve to none of the project's pages — carried with the `Negative`
label; an ambiguous case is `AmbiguousRegression` carried with `Confusable`,
mirroring how a claim pairs `Positive` with `Resolved`.

**Where each variant's entry point lives.** The `Resolved` case is
page-scoped: it names a page, so it stays on `EditPage`
(`claimScreen`/`classifyScreen`) and is authored from the Pages panel's
"record screen" button, unchanged. The two pageless variants name no page
and cannot sit on an `EditPage` (which is always opened against one page),
so they go through the free-function actions layer, respecting the split
already adjudicated for screen-scoped-but-pageless semantics:

- `recordScreenExpectation(AuthoringDraft, ScreenExpectationSpec)` in
  `authoring-edit.cpp` is the pure draft transaction, a direct sibling of
  `claimScreenForPage`. It takes a `PagelessExpectation` (`Unknown` or
  `Ambiguous`), rewrites the screen's single case rather than adding a
  second, sets the matching classification label, and fails when the screen
  is not part of the draft.
- `requestScreenExpectation(AppState&, PanelUiState&, SourceId,
  PagelessExpectation)` in `authoring-actions.cpp` is the panel-facing
  entry point: it mints the regression id, parks the validated draft on the
  one-commit-per-frame `requestEdit` queue, and reports a refusal on the
  status line (`screen <id> recorded as none of the pages` /
  `... as ambiguous`).

**UI.** The Properties panel's Regression section now always shows what the
shown screen is recorded as (resolves-to-page / none-of-the-pages /
ambiguous / nothing yet) and offers two buttons — "Record: none of the
pages" and "Record: ambiguous" — that work whether or not a case already
exists, so a screen with no case can be recorded straight into a pageless
variant. The classification-enum combo stays as a secondary relabel for an
existing case. Recording a `Resolved` case is deliberately left to the
Pages panel, since it needs a page the Properties panel does not pick.

## 2026-07-26 P0-A gap bundle (six items)

Six authoring-UI gaps closed on top of the per-placement ROI / regression /
ASan-smoke work in the same tree. Decisions worth recording:

- **Canvas evidence overlay.** The canvas now draws each matched rectangle the
  last Preview found on the shown screen (amber when the match passed its
  threshold, red when the closest match still missed), labelled with the short
  id. Data flow: the overlay reads `AppState::lastPreview()` — the same stored
  result the Actions panel prints as text — so there is no second owner of the
  evidence. `PreviewResult` gained a `sourceId`, set by `runPreview`, and the
  overlay draws only when it equals the shown screen, so a stale preview from
  another screen never paints boxes over unrelated pixels. Model-check margins
  are scores, not rectangles, so the canvas evidence comes from the Preview
  alone. Boundaries are snapped to whole screen pixels by a new pure
  `snappedScreenBounds` in `canvas-math` (tested).
- **Threshold in percent.** The Properties widget edits a percentage to two
  decimals; persistence stays integer basis points. Conversion is the pure pair
  `thresholdPercentFromBasisPoints` / `thresholdBasisPointsFromPercent` in
  `authoring-actions` (display = bp / 100; commit = round(percent × 100) clamped
  to [0, 10000], rounded in `double` so 99.99 survives). The display→commit
  round-trip of an unedited value is the identity for every basis point in
  range, pinned by a logic-layer test. No float reaches the document.
- **Duplicate.** `duplicateElement` mints an independent second element: fresh
  id, `freshAuthoringName(draft, original)` for a unique derived name, same
  template/ROI/threshold/click/kind/shared. It **inherits the original's
  placements**, retargeted to the new id — chosen over "unplaced on the current
  page" because the closure rule requires an interactive element to be placed on
  ≥1 page (a placeless copy of an action target could not build) and because v2
  models an element as one thing placed on N pages, so the copy is a new such
  thing with its own placement set. An anchor carries no placements, so its copy
  is an unassigned spare under "Not on any page". One undo removes it.
- **+ Info region.** `PageMemberKind` gained `InfoRegion`; `addPageMember` and a
  new `EditPage::placeInfo` route it through the existing placement path (info
  joins a page through a placement, no click offset). The Pages panel button sits
  next to "+ Interactive region".
- **Advanced removed.** The type combo moved up with the identity fields (after
  Name) and page membership moved to an "On pages" section near the top; the
  `CollapsingHeader("Advanced")` held only those two and is deleted.
- **Properties pointer lifetime.** `drawPropertiesPanel` copied the selected
  element's fields into a `SelectedElement` value snapshot at the top and no
  longer holds a `RecognizerDefinition const*` from the live document across the
  widgets; the sharing / type / membership helpers read the snapshot. Verified
  under the `x64-asan` `AsanSmoke` smoke.

## 2026-07-27 — the (element, page) → runtime recognizer mapping seam

The per-placement expansion above (2026-07-26 §7) made
`compileAuthoringDocument` turn an element placed on N ≥ 2 pages into N runtime
recognizers under **derived** ids, while the UI-facing `catalog()` keeps one
recognizer per element under the **element** id. That split had a latent
consequence the real machine surfaced: the workbench selects an action target
by its element id, then handed that id straight to
`RecognitionRuntime::evaluateActionTarget`. For a single-placed element the two
ids coincide and it worked; for a multi-placed element the element id is absent
from the runtime catalog, so the call failed with "recognizer … is not present
in the runtime catalog" — and because the preview surfaced that as a whole-run
error, `lastPreview()` stayed empty and the canvas drew **nothing**, including
the anchor boxes that had computed fine.

**The mapping is now one named seam.** The id derivation moved out of the
compiler's anonymous namespace to a public function,
`annotation::derivedRuntimeRecognizerId(elementId, pageId)`, declared beside
`compileAuthoringDocument`. The compiler calls it to emit the recognizers;
`preview.cpp` calls it to resolve an element id back to the runtime recognizer
for a given page. Single source of truth, so the two can never drift; the
generated manifest is byte-identical (the golden compiler test is unchanged),
and a new compiler test pins that each emitted per-page id equals what the
public function returns.

**Preview and model check resolve the page context, then map.** A single-placed
element (or an anchor) keeps its own id and is searched as before. A multi-placed
element is searched only through the recognizer for the previewed screen's page —
the page a regression case records for that screen. All evidence is filed under
the **element** id (the row's `recognizerId` is rewritten back), so nothing above
`preview.cpp` ever sees a derived id: the UI keys selection, rows, the canvas
overlay, and model-check margins by element id, unchanged.

**Skip semantics — the deliberate non-failure.** When the previewed screen is
unclaimed, or its claiming page does not place the element, there is no runtime
recognizer to evaluate (the element is neither authorized nor given a search
region there). The preview does **not** fail: the page and anchor evaluation
always runs, and `PreviewResult` carries a new `actionSkipNote` stating why
(`action preview skipped: "menu" is not placed on this screen's page`, with a
`— claim this screen to a page first` tail when the screen is unclaimed). The
status line after a successful preview now states the outcome concretely —
`preview: resolves to "…"; N hits, M misses drawn`, with the skip note or a
"a search hit its budget, so a box is missing" tail appended when either applies.

The model check maps the same way per screen: the recorded page is each captured
screen's context, and the live frame uses the page it resolved to. A multi-placed
element is evaluated only on a screen whose page places it — preserving the
existing intent that a mark is scored only where it is authorized (a screen whose
page does not place the element simply contributes no row for it, exactly as it
carries no search region there); a single-placed element is still searched on
every screen for the cross-screen misfire margin. A multi-placed element's score
lands under the element id, so `findMargin` resolves it unchanged.

## 2026-07-27 — per-search comparison budget at the workbench boundary

`RecognitionRuntime::evaluatePage` spends one `policy.maximumPixelComparisons`
across **every** anchor it searches, handing each search the remainder
(recognition-runtime.cpp). The workbench had been passing
`k_recognitionComparisonBudget` (256 Mi) as that whole-page total, so on the
real 4K project the first large search ROI (~270 M comparisons) exhausted the
entire budget and every later anchor reported a budget stop — the preview drew
zero evidence rows (`preview: page search hit its budget; 0 hits, 0 misses`).
The same starvation reached the model check's per-screen `evaluatePage` calls.
Meanwhile `scoreRegionOnScreen` already treats the constant as one search's
budget, which is why its "579% of budget" warnings measure a single search.

**The module's budget semantics are unchanged** — the release cli and engine
depend on the shared-remainder behavior, so nothing under `modules/` moved.
`k_recognitionComparisonBudget` is now, by declaration, the ceiling for **one**
search at the workbench edge. A new pure helper `pagePolicyFor(perSearchPolicy,
anchorSearchCount)` scales only `maximumPixelComparisons` by the anchor count
(checked multiply, saturating on the unreachable overflow) and copies the
deadline and cancellation through untouched. `evaluatePageOn` — the single point
every page evaluation passes through (preview, each captured screen, the live
frame) — derives the count from the runtime catalog's `pageAnchorOrder().size()`
(the same order `evaluatePage` iterates; placements never expand anchors, so the
derived catalog's anchor count equals the runtime's) and applies the scale.
Each `evaluateActionTarget` / `scoreRegionOnScreen` call is a single search and
keeps the per-search budget unscaled (×1). The deadline remains the wall-clock
guard.

**Status line names the stop.** `previewStatusLine` now reports the concrete
search and reason a `PreviewStop` carries — `preview: page search stopped at
"menu" (budget)` / `(deadline)`, and an equivalent `action search stopped at
"…" (…)` note — instead of a generic "hit its budget", since a budget stop and
a deadline stop have opposite fixes (shrink the search vs. wait longer).

Verified on the real project copy: `runPreview` under the GUI policy now
produces both anchors' evidence rows with no budget stop on both screens (the
matching anchor scores 0, the other misses by a wide margin), where before the
first search alone consumed the whole page total.
