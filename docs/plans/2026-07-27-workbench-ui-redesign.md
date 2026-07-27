# Workbench UI redesign

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
toolbar, or drawer (those stay U1b). Files: `app/workbench-app.{hpp,cpp}`,
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
`app/workbench-app.{hpp,cpp}` (staleness flags), new `project-tree.{hpp,cpp}`
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
