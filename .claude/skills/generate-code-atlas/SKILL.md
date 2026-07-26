---
name: generate-code-atlas
description: Regenerate the interactive HTML code atlas at docs/knowledge/atlas.html from module sources via agentic extraction, mechanical verification, and a Python renderer. Use when the user asks to 刷新/更新代码地图集, refresh or regenerate the code atlas, or when the atlas is stale after significant code changes (new/renamed public types, changed module boundaries).
---

# Generate Code Atlas

The atlas is a self-contained HTML reference (模块 → 类型 → 成员) rendered from
per-module JSON inventories in `scripts/generate_code_atlas/data/`. Prose is
Chinese; identifiers stay as-is. Every `file:line` links into VS Code via
`vscode://`, so line numbers must be real.

## Pipeline

```
extract (agents) -> data/<module>.json -> verify.py -> render.py -> docs/knowledge/atlas.html
hand-written tour_content.py -> render_tour.py -> docs/knowledge/tour.html
```

The tour (《深度导读》) is a hand-authored teaching companion: chapters in
`tour_content.py` embed verbatim code excerpts that `render_tour.py` verifies
against the sources at build time (exit 1 + DRIFT lines on mismatch). After
source changes that touch an excerpt, update the excerpt text/line numbers in
`tour_content.py`, then re-run `python scripts/generate_code_atlas/render_tour.py`.

Tour prose style (user-set, keep it): plain technical Chinese, like a good
engineering blog. Concrete scenario first, then the naive approach and its
failure, then the real code. Short declarative sentences; no aphorism-dense
"金句" stacking, no 「」-quoting every concept, no chained em-dashes, minimal
forward references. One memorable line per section at most.

## Quick start (data already up to date)

```bash
python scripts/generate_code_atlas/verify.py
python scripts/generate_code_atlas/render.py
```

`render.py --artifact-out <path>` additionally writes the content-only variant
for republishing the claude.ai artifact (URL lives in auto-memory
`code-atlas-artifact`).

## Workflows

### Incremental refresh (default)

1. Find which modules changed since the atlas was last generated:
   `git log --name-only` or `git diff --name-only <ref>` filtered to
   `modules/*/` and `entry/*`. Map paths to module keys (see
   `references/extraction.md`).
2. For each affected module, launch one reader agent (Workflow tool if several,
   a single Agent otherwise) with the extraction prompt template from
   `references/extraction.md`. Agents overwrite `data/<key>.json`.
3. Run `python scripts/generate_code_atlas/verify.py`:
   - `FIXED` lines were auto-corrected — nothing to do.
   - `BROKEN` lines must be resolved (wrong file, hallucinated name) before
     rendering; exit code 1 while any remain.
   - `GAP` lines list uncovered header declarations. Triage each with the
     backfill rules in `references/extraction.md`; private/nested types shown
     as members of a covered parent are fine to leave.
4. Run `python scripts/generate_code_atlas/render.py` and confirm it reports
   the expected module/class counts.
5. Small edits (one class added/renamed) may skip agents: edit the JSON by
   hand following the schema, then verify + render.

### Full rebuild

Same as above but extract all nine modules in one Workflow fan-out. Only needed
after large refactors or when the data files are missing/distrusted.

### Structural changes

When a module is added, removed, or a dependency edge changes: update
`MODULE_ORDER`, the `NODES`/`EDGES` coordinate tables (hand-laid SVG dependency
diagram), and `verify.py`'s `MODULE_HEADER_DIRS`, plus the module table in
`references/extraction.md`.

## References

- `references/extraction.md` — read before launching reader agents: module
  key/path/doc table, the exact JSON schema, the extraction prompt template,
  and the gap-backfill rules (A nested / B public / C tag-aggregate).
