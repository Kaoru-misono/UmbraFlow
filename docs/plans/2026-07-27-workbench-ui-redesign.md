# Workbench UI redesign

> **Superseded 2026-07-31: the GUI this plan redesigns no longer exists.**
> `b57b67b` archived the Dear ImGui + D3D11 shell — panels, window shell,
> texture cache, file dialog, one-shot capture source, and the imgui submodule.
> The deciding artifact is
> [`2026-07-31-annotation-model-capabilities.md`](2026-07-31-annotation-model-capabilities.md)
> §四之二.1, which retires the GUI and converts three GUI-only affordances into
> preconditions on the CLI: `placeExisting`/`shareRegionOnPage` (landed as
> `umbra-authoring page reference`), `setSearchRoi` per page (landed as
> `page reference --search-roi`), and the falsification matrix
> (`ModelCellCell` / `classifyModelCell`, still in `entry/workbench/preview.*`
> and still owed a CLI verb).
>
> Kept as history: the failure modes in "Why" are real observations about
> authoring, and the model-check reasoning informs the CLI matrix work. Nothing
> in the layout, docking, or panel sections is actionable.
>
> > **Extended 2026-07-31 (`f768e6c`).** `b57b67b` took the ImGui shell; the
> > *backend* units this plan's landed-stage records name went next. Deleted with
> > their tests: `workbench-app` (`AppState`, `AppState::Selection` — decision 3
> > in full), `panel-state` (`PanelUiState`, `ToolbarCommand`, `LogEvent` and the
> > bounded event history — decision 8's reporting seam), `authoring-actions`,
> > `canvas-math` (decision 9's geometry), `project-tree` (decision 2's screen
> > buckets), and `model-check-view`. `model-check-job` followed in the same
> > working tree. Every "Files:" list below therefore names paths that no longer
> > exist; they record what the stage touched at the time, not where to look.
> > What survives from this plan is `edit-page.*` / `page-view.*` (re-expressed:
> > `EditPage` now holds an `AuthoringDraft` by value and `commitSelecting` is
> > gone) and the falsification matrix in `preview.*`.

Status: reviewed (dual review 2026-07-27: Claude Opus "approve with changes",
Codex "rework"; both verdicts adjudicated below). Supersedes the five-window
layout. Builds on docs/plans/2026-07-26-page-centric-authoring.md.

## Why

The current UI is five flat dock windows (Screens, Pages, Canvas, Properties,
Actions) plus a one-line status string. Real sessions demonstrated its failure
modes: features hidden in collapsing headers went undiscovered; "preview
complete" masked a totally-empty preview; the author must mentally join
Screens to the Pages that claim them; creation exists only as panel buttons;
model-check results are unreadable text rows.

## Principle

The authoring loop is the interface's spine: ingest screens -> classify into
pages -> author elements -> place/share -> verify -> save/generate. Regions of
the screen map to stages of the loop, not to tables of the data model.

## Layout

- **Top toolbar** (slim): Save/Generate, Undo/Redo, capture, target picker,
  dirty indicator. The toolbar is drawn at the top but its state-changing
  commands execute AFTER applyPendingEdit in the frame, preserving the
  panels-then-commit-then-actions ordering the current Actions window relies
  on (an undo must still see the same-frame widget deactivation edit).
- **Left: unified project tree**, page-centric, replacing Screens and Pages.
- **Center: canvas** as the primary interaction surface.
- **Right: Inspector**, context-sensitive on the selection value.
- **Bottom: verify drawer**, tabbed (Evidence | Model | Log), resizable, with
  a one-line persistent summary when collapsed. The default dock layout is
  built programmatically (DockBuilder) on first run — there is no ini today
  and window renames alone cannot produce this arrangement.

## Decisions from review (binding)

1. **Pages nest a screen LIST, not one screen.** A page relates to screens
   through per-source regression cases and several screens may resolve to the
   same page; `claimedScreen`'s first-match is a display shortcut, not a
   cardinality. A page node nests "Regression screens" (all sources whose
   resolved case names it) plus an optional primary authoring sample (the
   `pageSampleSource` fallback), and its members.
2. **Screen buckets, not one Inbox.** Screens outside any page split into:
   *needs classification* (no regression case — the true to-do),
   *expected unknown*, and *expected ambiguous* (deliberately authored
   pageless outcomes; finished work, badged, never nagged).
3. **Selection becomes a typed value in AppState.** A variant-like selection
   — Screen(sourceId) | Page(pageId) | Element(recognizerId, optional
   pageContext) — replaces the implicit pair of selectedSourceId +
   selectedRecognizerId. A tree row under page P selects
   Element(id, pageContext=P) explicitly; the canvas and Inspector take the
   page context from the selection first and fall back to the shown screen's
   claim only when the selection carries none. This is a deliberate
   logic-layer change (the U1 phase is NOT panel-only).
4. **Shared palette and orphan bucket stay separate groups.** "Shared
   regions" is a live drag-source palette; "Not on any page" is a re-homing
   bucket. Merging them puts a tool next to a junk drawer.
5. **The verify drawer owns its staleness.** lastPreview/lastModelCheck are
   aggressively invalidated (any edit, undo, redo, screen change). Each tab
   renders one of three explicit states: results / stale ("the project
   changed — re-run") / empty ("not run yet"), never silently blank.
6. **The full marks-x-screens matrix is deferred.** ModelCheck stores only
   each mark's own score and nearest-other margin; the per-cell grid does not
   exist and is a worker/result-layer extension. The first release ships the
   two tables the data supports: screen verdicts and per-mark margin
   ("headroom") rows. The grid is a named follow-up, not a phasing footnote.
7. **The severity log is a structured feature, shipped first.** appendLog
   gains a severity (info/warning/error) threaded from the call sites — no
   string sniffing — plus a bounded in-memory event buffer (severity,
   timestamp, message) in PanelUiState feeding the Log tab; the on-disk line
   format gains the severity word. This touches services and action-reporting
   paths and is scoped as its own phase (U0).
8. **Draw-to-create needs an atomic creation API.** EditPage gains a creation
   spec carrying kind, template rect, and initial placement ROI (one
   transaction — the one-commit-per-frame queue rejects a second same-frame
   edit, so create-then-retemplate cannot work). The gesture is available
   only on a screen with a page context; on an unclassified screen the
   affordance is inert with a message naming the fix ("classify this screen
   first"). Regression recording lives on screen context (screen row, canvas
   background menu, screen Inspector), never on an element's menu.
9. **Canvas interaction is a specified state machine.** Drawing and
   hit-testing extend from the selected element to every member of the shown
   screen's page. Arbitration on the single canvas surface, in order:
   selected element's grips -> click-hit any drawn rect to select (overlap
   cycles on repeated click) -> left-drag on empty space rubber-bands a new
   rect (click-vs-drag threshold) -> middle-drag pans, wheel zooms. Esc
   cancels a rubber-band; the type picker appears on release. The
   shared-region drag-drop (cross-frame ui.draggedRegion payload, deferred
   drop application) is re-hosted onto tree rows intact.
10. **Visible navigation controls.** Zoom indicator with fit/100% buttons on
    the canvas — wheel-only zoom is itself a hidden feature. Explicit empty
    states for tree, canvas, and drawer on an empty project. The dirty dot
    inherits the known cpp-debt (dirty stays set after undo-to-saved) and
    will over-report until that debt is paid; accepted.

## Phasing (revised)

- **U0 — structured message log.** Severity through appendLog + call sites,
  ring buffer, Log tab (initially as its own dock window if the drawer is not
  yet built). Institutional fix, independently shippable and testable.
- **U1 — information architecture.** Typed selection in AppState; unified
  tree (decisions 1, 2, 4); toolbar with post-commit execution; verify drawer
  with Evidence/Model tabs over existing data (decisions 5, 6); DockBuilder
  default layout. Logic-layer surface: the selection type and its adopters.
- **U2 — canvas interaction.** Decision 9's state machine; atomic creation
  API (decision 8); visible zoom controls; context menu routing edits through
  the existing queue (at most one edit per frame, selection immediate).
- **U3 — model-check grid.** Extend the worker result with per-(mark, screen)
  observations and explicit cell states, then the matrix widget with frozen
  headers and scrolling. Separately: full-text evidence table polish.

## Non-goals

Multi-select (the selection model stays single); a new UI framework (ImGui +
docking is locked); OCR/info-region semantic reading (deferred by design);
changing the annotation module's recognition or budget semantics.

## Execution log

### 2026-07-27 — U0 structured message log (shipped)

- `LogSeverity` (Info/Warning/Error) now lives at namespace scope in
  `panel-state.hpp`. `PanelUiState::report(severity, message)` is the single
  reporting seam; every report site across `app/panels.cpp`,
  `authoring-actions.cpp`, and `model-check-view.cpp` states its severity
  explicitly rather than the shell sniffing the message text. `statusLine`
  stays as the transient display value `report` writes.
- `PanelUiState` gained a bounded event history (`std::deque<LogEvent>`,
  capacity 256, oldest dropped) and `captureLogEvent`, which owns the
  consecutive-duplicate collapse and the bounding and returns the recorded
  event for the disk mirror. `PendingEdit` carries a `severity` so a deferred
  edit's description reports at the right level (a shared placement that does
  not match its screen lands as Warning).
- The on-disk line is now `{timestamp}  {SEVERITY}  {message}`; `appendLog`
  takes `(LogSeverity, timestamp, message)` and the shell composes the line via
  the pure `formatLogLine` helper. The panels compute the timestamp once
  (`formatLogTimestamp`) so the in-memory entry and the disk line share a stamp.
- A new "Log" dock window renders the history newest-at-bottom with per-severity
  colour (error red, warning amber, info default), a clear button that empties
  the in-memory buffer only, and auto-scroll that yields when the user scrolls
  up.
- Classification rule applied at each site: a failure / refusal / rejection
  ("… failed: …", "… rejected: …", a refusal naming a fix) is Error; a
  degraded-but-done outcome (a placement that does not match, a model check with
  wrong or deadline-stopped screens) is Warning; every other outcome is Info.

### 2026-07-27 — U1a typed selection (shipped)

Decision 3 only: the selection became a typed value in `AppState`; no tree,
toolbar, or drawer (those stay U1b). Files: `workbench-app.{hpp,cpp}`,
`authoring-actions.{hpp,cpp}`, `panel-state.hpp`, `app/panels.cpp`,
`tests/workbench/test-workbench-app.cpp`, `tests/workbench/test-edit-page.cpp`.

- `AppState::Selection` is a nested variant over
  `Screen(sourceId) | Page(pageId) | Element(recognizerId, shownScreen?,
  pageContext?) | nothing`. It replaces the `m_selectedSourceId` +
  `m_selectedRecognizerId` pair as the single source of truth. `Page` is a new
  capability with no UI producer yet (the U1b tree will select it).
- Invariants chosen and documented at the type:
  - **Selecting a Screen replaces the whole selection**, so it clears any
    element — a screen and an element are never both selected. (This is the one
    transition that reads differently from the old independent-axes pair, and it
    is pinned by a test.)
  - **An Element carries the screen it is shown over** so the canvas has an image
    to draw. When a selecting action names no shown screen, `select()` inherits
    the currently shown one, reproducing the old
    `setSelectedRecognizerId`-without-a-source behaviour (follow a created entity,
    leave the shown image where it was).
  - **`lastPreview` is dropped iff the shown screen changes.** The old
    `setSelectedSourceId` early-return-on-equal generalises to "reset when the
    derived shown screen differs", which also covers the element paths exactly as
    before (an element that follows to a new screen invalidates; reselecting the
    same screen does not).
  - **Reconcile degrades in place:** a deleted element falls back to the screen
    it was shown over (or nothing when that screen is gone too), a deleted screen
    or page falls to nothing, and a surviving element drops a shown screen or
    page context that vanished.
- Migration inventory: derived reads `selectedSourceId()` /
  `selectedRecognizerId()` stayed (now computed from the typed value), plus a new
  `selection()` accessor. WRITE sites moved to the single `select(Selection)`
  setter: the screen rows and page-sample button in `panels.cpp`,
  `addIngestedSource`, `applyPendingEdit`, and `selectRecognizer`.
  `PendingEdit::selectRecognizer/selectSource` collapsed into one
  `std::optional<AppState::Selection>`.
- Page-context precedence: `placementContext` and the canvas gained a
  selection-page argument that wins over the shown screen's claiming page;
  `selectRecognizer` gained the `pageId` its page-member-row callers already
  know, so a member row selects `Element(id, pageContext=that page)`. Intended
  behaviour change (pinned by a test): ROI editing on an element selected under a
  page now uses that page even when the shown screen's claim is ambiguous or
  missing.

### 2026-07-27 — U1b information architecture (shipped)

The rest of U1 minus the already-shipped typed selection: the unified tree,
the top toolbar with post-commit execution, the tabbed verify drawer, the
context-sensitive inspector, and the programmatic default dock layout
(decisions 1, 2, 4, 5, 6 and the Layout section). Files: `app/panels.cpp`
(the whole panel set and frame orchestration rewritten), `panel-state.hpp`
(`ToolbarCommand`, queued command + capture/import flags), `authoring-actions.
{hpp,cpp}` (`requestToolbarCommand` / `dispatchToolbarCommand`),
`workbench-app.{hpp,cpp}` (staleness flags), new `project-tree.{hpp,cpp}`
(pure bucket + regression-screen derivations), `entry/CMakeLists.txt` and
`tests/CMakeLists.txt`, and tests `test-project-tree.cpp` (new),
`test-workbench-app.cpp`, `test-authoring-actions.cpp`.

- **Old window → new home inventory.** The five flat windows (Screens, Pages,
  Canvas, Properties, Actions) plus U0's Log became: **Toolbar** (Save/Generate,
  Undo, Redo, target title + Capture, Import, dirty dot, persistent status
  line); **Project** (screen buckets + page tree, replacing Screens and Pages);
  **Canvas** (unchanged, still U2's surface); **Inspector** (Properties body
  under an Element selection, the regression section under a Screen selection,
  a summary under a Page selection); **Verify** (bottom drawer, tabs
  Evidence/Model/Log — Preview + verdict, the Check buttons + screen-verdict and
  mark-margin tables, and U0's log view). The standalone Log window is gone.
- **Screen buckets (decision 2).** `project-tree.cpp` classifies each screen
  from its regression case: no case → *Needs classification* (the to-do, count
  badge, always shown); `UnknownRegression` → *Expected unknown*; `Ambiguous
  Regression` → *Expected ambiguous* (badged, hidden when empty, never a nag); a
  resolved case → owned by its page. `regressionScreensForPage` lists every
  screen resolving to a page, so a page nests a screen *list* (decision 1) with
  the primary authoring sample marked and the record-screen affordance kept for
  an inferred-only sample.
- **Toolbar post-commit ordering.** The toolbar draws at the top but queues
  Save/Undo/Redo through `requestToolbarCommand`; `dispatchToolbarCommand` runs
  them after `applyPendingEdit`, and capture/import (which need the platform via
  `WorkbenchServices`) are flagged and drained by the shell in the same slot.
  An undo therefore reverses the same-frame widget-deactivation edit rather than
  racing it; pinned by a test at the action layer with no ImGui.
- **Drag-drop re-host (decision 4).** The shared-region palette moved below the
  pages as its own group, kept separate from the *Not on any page* orphan
  bucket. The cross-frame `ui.draggedRegion` payload, the per-page drop targets,
  and the deferred one-commit-per-frame drop application are intact; the palette
  drawing after its drop targets is safe because the drag payload persists across
  frames and the identity was recorded on earlier drag frames.
- **Staleness flags (decision 5).** `AppState` gained `m_previewInvalidated` /
  `m_modelCheckInvalidated`, set by `invalidatePreview` / `invalidateModelCheck`
  only when a result actually existed (edit, undo, redo, and — preview only —
  a shown-screen change), and cleared by `setLastPreview` / `setLastModelCheck`.
  The drawer reads result-present / flag-set / neither as results / stale /
  empty. Pinned by tests, including that an invalidation before any result does
  not read as stale and that a screen change stales only the preview.
- **Default dock layout.** `buildDefaultLayout` splits the dockspace (toolbar
  top, project left, inspector right, verify bottom, canvas centre) on the first
  frame only when `DockBuilderGetNode` finds an empty leaf — i.e. no layout was
  restored from the ini the shell already persists to LOCALAPPDATA. A user's
  saved arrangement wins; the persistence-free smoke run gets the default. No new
  ini handling was added (the shell already writes one).
- **Deferred:** the marks×screens matrix stays U3 (decision 6); the dirty dot
  still over-reports after undo-to-saved (accepted cpp-debt); canvas interaction
  is U2 and untouched.
- **Gates:** 4 static gates OK; x64-debug clean; `ctest -L CI` 14/14; asan
  rebuilt + `ctest -L AsanSmoke` 2/2 (the smoke draws every new panel under
  ASan); probe-project `--smoke 5` EXIT 0; x64-release workbench builds.

### 2026-07-27 — U2 canvas interaction (shipped)

Decision 9's state machine, decision 8's atomic creation, decision 10's visible
navigation, and the canvas context menus. Files: `app/panels.cpp` (the whole
canvas surface rewritten plus context-menu / creation / retype helpers),
`canvas-math.{hpp,cpp}` (the new pure geometry), `panel-state.hpp`
(`CanvasGesture` + its in-progress data), `edit-page.{hpp,cpp}`
(`NewDrawnMemberSpec` / `AddedMember` / `placeDrawn`), `authoring-actions.
{hpp,cpp}` (`shownPageForScreen`, extracted from `placementContext`), and tests
`test-canvas-math.cpp`, `test-edit-page.cpp`, `test-authoring-actions.cpp`.

- **Interaction state machine (decision 9).** `PanelUiState::CanvasGesture` is
  the explicit mode, one active at a time: **Idle** (a fresh left press is
  arbitrated from here), **GripEditing** (a grip of the selected element is held;
  the detail stays in the existing `dragTarget`/`dragGrip`/`dragStartRect`, and
  this state mirrors `dragTarget != None`), **PressPending** (left down on empty
  canvas, undecided below the drag threshold), **RubberBanding** (past the
  threshold, dragging a new rectangle). Transitions: Idle→GripEditing when
  `handleRectEditing` claims a grip (unchanged); Idle stays Idle on a template hit
  and selects it, cycling on overlap; Idle→PressPending on an empty press where a
  shown page exists; PressPending→RubberBanding past `k_dragThreshold` (4 px),
  else →Idle on release; RubberBanding→Idle on release (opening the type picker
  when a rectangle was drawn); any→Idle on Escape (which also clears a grip drag).
  Left-press arbitration order is grips → template hit (select/cycle) →
  empty-space rubber-band; middle-drag pan and wheel zoom are untouched.
- **Drawing (decision 9).** The canvas draws every member of the shown page —
  `shownPageForScreen` picks the page an element is selected under, else the one
  claiming the shown screen — in a muted style (template + this-page search ROI as
  outlines), the selected one strong with grips via the unchanged
  `handleRectEditing`, evidence overlay on top. Hit-testing is against template
  rectangles only, since a whole-frame search ROI would swallow every empty press
  and leave nothing to rubber-band on.
- **Hit-test / cycling (pure).** `rectsUnderPoint` returns the rectangles under a
  source point smallest-area first (a mark on a region is reachable and cycles
  ahead of it); `nextRectInCycle` advances past the current selection among the
  hits, wrapping, so repeated clicks on coincident rectangles cycle.
  `exceedsDragThreshold`, `rubberBandRect`, `searchRoiForDrawnTemplate`,
  `fitCanvasView`, and `centeredCanvasView` complete the tested pure layer.
- **Atomic creation API (decision 8).** `EditPage::placeDrawn(NewDrawnMemberSpec
  {sourceId, kind, templateRect})` is one transaction: it derives the initial
  per-page search ROI from the drawn template (grown by the template's own extent
  on every side, clamped to the frame — always containing it) rather than seeding
  the whole frame, refuses a template below `k_minimumDrawnTemplateExtent` (2 px),
  mints the member, and returns `AddedMember{id, name, kind}` for
  select-after-landing. On rubber-band release a popup offers anchor / interactive
  / info / cancel; on a screen with no page context the rubber-band never starts
  and a one-shot Info names the fix. Refusals surface through `ui.report(Error)`.
- **Context menus (decisions 8, 9).** Right-click on a drawn template selects it
  and opens an element menu — Duplicate, a Retype submenu (through the shared
  `requestRetype`, which the inspector's type combo now also calls), Remove from
  this page (refused for an anchor and for an interactive element's last
  placement, with the closure message), Delete everywhere — never regression
  recording. Right-click on the background opens the screen-scoped
  `requestScreenExpectation` recordings. Every menu action requests at most one
  edit; selection is immediate.
- **Visible navigation (decision 10).** A footer strip on a reserved line (so its
  buttons never overlap the canvas surface and double-fire a press) shows the zoom
  percent and Fit / 100% buttons over the pure `fitCanvasView` /
  `centeredCanvasView` math; wheel and middle-drag are unchanged.
- **Deferred:** none for U2. The dirty dot still over-reports after undo-to-saved
  (accepted cpp-debt from U1); the marks×screens matrix stays U3.
- **Gates:** 4 static gates OK; x64-debug clean; `ctest -L CI` 14/14; asan
  rebuilt + `ctest -L AsanSmoke` 2/2; probe-project `--smoke 5` EXIT 0;
  x64-release workbench builds.

### 2026-07-27 — U3 model-check grid (shipped)

Decision 6's deferred follow-up: the worker result now carries the full
marks×screens grid, and the Model tab renders it as a matrix beneath the two
existing tables. Files: `preview.{hpp,cpp}` (cell model, derivation, pure
classifier), `model-check-view.{hpp,cpp}` (`findModelCell` lookup),
`app/panels.cpp` (`drawModelMatrix` and the cell colour/text/tooltip helpers),
and tests `test-preview.cpp`, `test-model-check-view.cpp`.

- **Cell model.** `ModelCheck` gained `std::vector<ModelCheckCell> cells`, one
  per (element, screen) over the captured screens, filed under the ELEMENT id
  (never the derived per-page recognizer id) so the existing element-keyed
  lookups reach it. Each cell carries a `ModelCellOutcome` — `Hit` / `Miss`
  (measured, with `sadScore` and `maximumSad`), `Stopped` (with the
  `SadSearchStopReason` for the tooltip), or `NotSearchedHere` — plus the
  ground truth the outcome is read against. The margins and the screen verdicts
  are untouched; the two current tables and `findMargin` / `findScreenCheck`
  keep working unchanged.

  **Superseded 2026-07-31.** That ground truth was a `bool expectedHit`, "the
  screen's recorded page references the element", and both halves of it were
  wrong. It ignored the reference's `SignatureRole`, so a page that FORBIDS a
  mark expected it to hit — a correctly absent mark read as a hole, and the
  broken case the role exists to catch read as expected and vanished. And it
  assumed pages occupy disjoint screens, so every element of an overlay page
  (a card-detail screen is the battle screen with a card selected) reported a
  misfire for being genuinely on screen. It is now a three-state
  `ModelCellExpectation` — `Match` / `Absent` / `Unclaimed` — derived from what
  the model actually states: the recorded page's own reference including its
  role, and failing that, the duty another page's signature leaves resting on
  the mark when every OTHER clause of that signature holds on the screen. A
  signature is a conjunction, so a page kept off a foreign screen by its
  forbidden clause demands nothing of its other members there. `expectedHit` is
  gone: it survived one day as a `TODO(cpp-debt)` mirror for the authoring CLI's
  `expected_hit` field, and both were removed when `check` began answering with
  the three states by name (`"expectation": "match" | "absent" | "unclaimed"`).
  A bool cannot separate them, and the difference is the reader's instruction:
  under `absent` a hit is a defect to repair, under `unclaimed` it is the same
  element genuinely on a screen no page's identity rests on. An element located
  by its page — one declaring no appearance — takes part in no signature, so it
  produces `unclaimed` on every screen its own pages do not claim.
- **NotSearchedHere is explicit.** A multi-placed element on a screen whose page
  does not place it is a distinct cell state, not an empty hole. Anchors and
  single-placement elements are searched on every screen, so they never take it.
- **Derived without a second pass.** The cells come from exactly the per-screen
  evaluation the margins already fold in: the page anchor rows (and any page
  stop) for anchors, and the action rows for interactive elements. The action
  evaluation, which previously discarded stopped searches, now returns them
  alongside the rows (`ActionEvaluation`) so a stopped action becomes a
  `Stopped` cell rather than a silent gap. `deriveScreenCells` reads that data;
  no recognizer is searched twice. An anchor with no row is `Stopped` when the
  page stopped and `NotSearchedHere` otherwise (an orphan anchor on no page).
- **Thin-margin rule.** The margin view carried no existing "thin" definition to
  inherit, so the documented choice is 10% of the threshold: a measured score
  whose distance from `maximumSad` is within `maximumSad / 10` reads amber.
  `classifyModelCell` is a pure, total function over one cell — precedence is
  wrong-outcome (red: a hit off the element's pages, or a miss on a screen its
  own page owns) over stopped-or-thin (amber) over clear-and-correct (green),
  and NotSearchedHere is neutral/dim — so a misfire never hides behind a thin
  band. `ModelCellColor` keeps the classification one step removed from ImGui.
- **Matrix widget.** `drawModelMatrix` renders an ImGui table with the mark
  column and the screen header frozen (`TableSetupScrollFreeze(1,1)`), bounded
  to 260 px of scrolling height for 1080p, rows taken from the same margins the
  tables above use (so info regions are absent here too). Each cell fills with
  its classified colour, shows the score share, and hovers a tooltip (outcome
  word + score vs threshold). Clicking a cell reuses `selectRecognizer` to
  select the mark and follow to that screen, passing the screen's page as
  context only when that page places the mark. The tab keeps its three states.
- **Tests.** `test-preview.cpp`: `classifyModelCell` as a table of outcomes
  (expected/thin/misfire/not-searched across hit and miss); the grid filled for
  a mark searched on every screen (hit own, clean miss elsewhere); a
  multi-placed element not-searched off its pages; a stopped anchor recorded as
  a stopped cell. `test-model-check-view.cpp`: `findModelCell` by element and
  screen, and absent for an uncovered pair. Existing margin/verdict tests pass
  unmodified.
- **Deferred:** the full-text evidence-table polish noted beside the grid in the
  U3 phasing line is not part of this change. The dirty dot still over-reports
  (accepted cpp-debt from U1).
- **Gates:** 4 static gates OK; x64-debug clean; `ctest -L CI` 14/14; asan
  rebuilt + `ctest -L AsanSmoke` 2/2; probe-project `--smoke 5` EXIT 0;
  x64-release workbench builds.

### 2026-07-27 — U4 polish (shipped)

Four user-approved items closing the redesign: keyboard shortcuts, the
revision-based dirty dot, in-tree rename, and a delete-everywhere confirmation.
Files: `app/panels.cpp` (shortcuts, rename helpers, the confirmation modal,
`performPreview` extracted from the Evidence tab, and the entry-point rewiring),
`panel-state.hpp` (`RenameKind`, `InlineRename`, `PendingDelete`, and the
`previewRequested` / rename / confirmation fields), `authoring-edit.{hpp,cpp}`
(`AuthoringEditHistory::position`), `workbench-app.{hpp,cpp}`
(`m_savedPosition` replacing the latched `m_dirty`), and tests
`test-workbench-app.cpp`, `test-authoring-edit.cpp`.

- **Keyboard shortcuts.** `handleShortcuts` runs before the panels each frame:
  Ctrl+S / Ctrl+Z / Ctrl+Y queue a `ToolbarCommand` through the same
  `requestToolbarCommand` the toolbar buttons use (dispatched after
  `applyPendingEdit`), F5 flags `previewRequested` that the shell drains
  post-commit through the extracted `performPreview` (so it scores the committed
  document, like the Evidence tab's own button), and Delete opens the
  delete-everywhere confirmation for the selected element or leaves an Info hint
  when nothing deletable is selected. All are suppressed while
  `io.WantTextInput` is set (a text field, including the inline rename) or any
  popup or menu is open (`IsPopupOpen` with the any-popup flags), so typing a
  name or answering the confirmation never fires an action.
- **Revision-based dirty (decision 10's inherited debt, paid).**
  `AuthoringEditHistory` now pairs each undo/redo snapshot with a monotonic
  *position* identity that undo and redo restore (distinct from `revision()`,
  which only advances and still guards stale commits). `AppState` records the
  saved position on load and in `markSaved`, and `dirty()` is the current
  position differing from it, so undo-to-saved reads clean and redo past it
  reads dirty again. The `TODO(cpp-debt)` in `dirty()` and the stale toolbar
  comment are gone; no cpp-debt ledger entry existed to remove.
- **In-tree rename.** Double-clicking a page header or a member/element row
  opens an inline `InputText` (single `InlineRename` slot in `PanelUiState`,
  seeded from the current name, focused on its first frame). Enter commits
  through the same draft-edit path the Inspector's element rename uses -- a
  duplicate or empty name is refused by the build and surfaced through
  `applyPendingEdit`'s `report`; Esc or a click away cancels. Single-click
  select is intact (the page node dropped `OpenOnDoubleClick` so the
  double-click is free to rename; the arrow still toggles).
- **Delete confirmation.** Every delete-everywhere surface -- the tree Remove
  button, the canvas element menu, the new Inspector Delete button, and the
  Delete key -- routes through `requestDeleteEverywhere`, which reports a delete
  path's own refusal immediately (no pointless confirmation) and otherwise opens
  a modal naming the element and the placements or signatures it withdraws.
  Confirm runs the existing `requestDeletion` (committed the same frame), Cancel
  does nothing, and a selection moving off the element abandons the pending
  confirmation. "Remove from this page" stays unconfirmed.
- **Deferred:** none. Rename and confirmation are ImGui-bound and kept thin over
  the already-tested actions, so they carry no new logic tests; the
  revision-based dirty and `position()` are covered.
- **Gates:** 4 static gates OK; x64-debug clean; `ctest -L CI` 14/14; asan
  rebuilt + `ctest -L AsanSmoke` 2/2; probe-project `--smoke 5` EXIT 0;
  x64-release workbench builds.
