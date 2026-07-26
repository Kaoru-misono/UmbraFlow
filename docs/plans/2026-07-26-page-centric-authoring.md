# Page-centric authoring: domain model v2 and the editing layer

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

`MemberId` is the page-local key. In phase 1 it **is** `RecognizerId` —
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
and call handles. `MemberId` is `RecognizerId` in this phase, so
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
