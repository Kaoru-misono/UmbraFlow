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
