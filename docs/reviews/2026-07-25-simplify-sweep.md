# Repo-wide C++ simplification sweep — 2026-07-25

> Status: **Report-only (2026-07-25)**. No C++ source was changed. This is the
> single deliverable of a repo-wide reuse/simplification sweep over the
> first-party C++ under `modules/` and `entry/`. Every item below is a
> quality (reuse/simplification/dead-code) cleanup, not a correctness fix, and
> each survived adversarial verification at score >= 80.

## 1. Summary

The sweep covered **191 first-party `.cpp`/`.hpp` files** under `modules/` and
`entry/` (vendored `external/` excluded). Each of six file groups was reviewed
independently by **two models** (Claude and Codex), findings were **scored
0-100 by a verifier instructed to refute by default**, and everything below
**80 was dropped**. **9 findings survived** (from 60 candidates across the
groups). Estimated **net removable is ~240 lines** (the two annotation findings
describe the same dedup, so their raw 90+80 counts as ~90 once). The three
highest-value items are: **(1)** collapsing the duplicated annotation TOML
field codec shared between `authoring-document.cpp` and `runtime-manifest.cpp`
(~90 lines), **(2)** folding the three keyboard-transition copies in
`modules/controller/source/controller/input.cpp:357` into one helper (~50
lines), and **(3)** replacing the four near-verbatim teardown blocks in
`modules/controller/source/controller/platform/windows-capture.cpp:1020` with
one lambda (~32 lines).

## 2. Cross-cutting patterns

Fixing a pattern once is worth more than editing each site. Three patterns
account for 7 of the 9 findings.

### Pattern A — Duplicated annotation TOML field codec (fix-once, shared TU)

The authoring codec and the runtime codec independently serialize/parse the
same annotation domain fields. Several helpers are **byte-identical** copies;
the parse helpers differ **only** in the `"authoring document"` vs
`"runtime manifest"` message prefix.

Sites:
- `modules/annotation/source/annotation/authoring-document.cpp:560`
  `annotationTypeText` == `modules/annotation/source/annotation/runtime-manifest.cpp:289`
- `modules/annotation/source/annotation/authoring-document.cpp:617`
  `appendRectField` == `runtime-manifest.cpp:304`
- `modules/annotation/source/annotation/authoring-document.cpp:635`
  `appendIdArray<Id>` == `runtime-manifest.cpp:268`
- `modules/annotation/source/annotation/authoring-document.cpp:244`
  `parsePixelRectField` ~= `runtime-manifest.cpp:48` (prefix-only diff)
- `modules/annotation/source/annotation/authoring-document.cpp:278`
  `parseAnnotationType` ~= `runtime-manifest.cpp:106` (prefix-only diff)

Single change: move `appendRectField`, `appendIdArray`, and `parsePixelRectField`
into `detail/canonical-toml.{hpp,cpp}` (both TUs already include it), with
`parsePixelRectField` taking the caller's document label so diagnostics stay
identical; move `annotationTypeText` + `parseAnnotationType` next to
`enum class AnnotationType` in `catalog.{hpp,cpp}` as one shared wire-name
mapping. Delete both private copies. Resolves findings id 0 and id 6 (same
work, reported by both models).

### Pattern B — Local wrappers re-implementing an available API (rule drift)

Small file-local helpers re-derive behaviour a standard, `core`, or catalog
facility already provides. The single rule: **delete the wrapper, call the
existing facility.** Spans the m0-demo and workbench groups.

Sites:
- `entry/m0-demo/main.cpp:36` and `entry/m0-demo/capture-mode.cpp:32` —
  verbatim `dpiDeclarationName` switch reimplements `core` `enumName`. Add
  `UF_REFLECT_ENUM(uf::DpiDeclaration, ...)` to `controller/dpi.hpp` (the
  pattern `domain/error.hpp` already uses for `AutomationErrorKind`) and call
  `enumName(dpiDeclaration).value_or("Unknown")`; delete both helpers.
- `entry/workbench/app/panels.cpp:79` — `containsId` is an exact spelling of
  `std::ranges::contains` (used elsewhere in the file); replace the calls at
  `panels.cpp:1069`, `panels.cpp:1106`, `panels.cpp:1110` and delete it.
  `pageName` hand-scans `catalog.pages()` although
  `RecognitionCatalog::findPage(PageId)` exists — implement it via
  `state.document().catalog().findPage(id)` (keep the short-id fallback) and
  delete the manual loop.

### Pattern C — Repeated sequential blocks that collapse to one local helper

The same "N near-verbatim blocks in one function" shape recurs across the
controller and cli groups. Each site collapses to its own file-local helper
(these are independent helpers, so this is a shared *shape*, not one edit — the
individual entries live in the group worklists below). Reviewer guidance is
shared: extract the varying pieces into parameters, keep block order and
error-accumulation semantics unchanged.

Sites:
- `modules/controller/source/controller/platform/windows-capture.cpp:1020` —
  four close/revoke blocks → one `step(open, ctx, action)` lambda.
- `modules/controller/source/controller/input.cpp:357` —
  `keyPress`/`keyDown`/`keyUp` → one `deliverKeyTransition(...)` helper.
- `entry/cli/args.cpp:69` — `parseMilliseconds`/`parseSeconds` +
  double-parsed `--poll` → one `parseDurationCount<Unit>(...)`.

## 3. Worklist by group

Findings folded into Pattern A or Pattern B are omitted from their group tables
(they are resolved by the cross-cutting change). Pattern C findings appear in
their group tables because each needs its own helper.

**core-domain** (`modules/core`, `modules/domain`) — 0 kept of 10. Nothing to do.

**engine-image-vision-script** (`modules/engine`, `modules/image`, `modules/vision`, `modules/script`) — 1 kept of 8.

| file:line | category | claim | proposal | lines | risk | score |
|---|---|---|---|---|---|---|
| `modules/vision/source/vision/sad.cpp:35` | dead-code | `checkedSubspan` re-derives `lastIndex=*end-1` and re-checks via `tryAt` after the `*end > data.size()` guard already proved it in-bounds | Delete lines 35-45; `data.subspan(offset, count)` at line 47 is already valid (count==0 is short-circuited by `count!=0 &&`). `checkedSubtract`/`tryAt` stay used elsewhere, no include changes | 11 | local | 80 |

**annotation** (`modules/annotation`) — 2 kept of 12. Both findings are **Pattern A**; see cross-cutting section.

**controller** (`modules/controller`) — 2 kept of 11. Both are **Pattern C** (own-helper each).

| file:line | category | claim | proposal | lines | risk | score |
|---|---|---|---|---|---|---|
| `modules/controller/source/controller/platform/windows-capture.cpp:1020` | simplification | `teardownUnlocked` repeats the guard/`winrtCall`/clear-flag/`retainFirstError` shape 4x for `m_sessionOpen`, `m_frameArrivedRegistered`, `m_itemClosedRegistered`, `m_framePoolOpen` | Add `auto step = [&](bool& open, string_view ctx, auto&& action){ if(!open) return; auto r=winrtCall(ctx,action); if(r) open=false; retainFirstError(std::move(r)); };` and replace the four blocks with four `step(...)` calls in the same order; final `clearLatestFrame`/`m_windowMarker.close()` stay | 32 | local | 82 |
| `modules/controller/source/controller/input.cpp:357` | simplification | `keyPress`/`keyDown`/`keyUp` repeat generation + held-target + held-state + window-liveness validation, scan-code lookup, `PostSpec` build, delivery and `HeldInputs` mutation | Add `deliverKeyTransition(target, actionGeneration, key, transition, held, audit)` that validates the expected held state, posts `keySpec(...)`, applies `onKeyDown`/`onKeyUp`. Make `keyDown`/`keyUp` one-line delegates; implement `keyPress` as Down then Up (mind the double precondition check) | 50 | local | 80 |

**m0-demo** (`entry/m0-demo`) — 2 kept of 8. `dpiDeclarationName` is **Pattern B**; the dead function is standalone.

| file:line | category | claim | proposal | lines | risk | score |
|---|---|---|---|---|---|---|
| `entry/m0-demo/capture-output.cpp:124` | dead-code | `captureFramePng(session, path)` is declared and defined but never called (capture-mode.cpp inlines `session.capture()`+`writeFramePng`; input-agent.cpp has its own `captureToOutput`) | Delete the definition (`capture-output.cpp:124-132`) and declaration (`capture-output.hpp:33-37`); sibling `writeFramePng`/`encodeFramePng` stay in use | 13 | mechanical | 82 |

**workbench-cli** (`entry/workbench`, `entry/cli`) — 2 kept of 11. `panels.cpp` is **Pattern B**; `args.cpp` is **Pattern C**.

| file:line | category | claim | proposal | lines | risk | score |
|---|---|---|---|---|---|---|
| `entry/cli/args.cpp:69` | simplification | `parseMilliseconds`/`parseSeconds` are structurally identical (unsigned parse, max calc, checked cast, error build, `duration_cast`); `parsePollInterval` parses the `--poll` token twice (line 104 then 120) | Introduce `parseDurationCount<Unit>(uint64 count, string_view flag, string_view unitName)`. Parse each option once via `parseUnsigned`, pass the count in; let `parsePollInterval` validate bounds then pass the already-parsed count through; delete `parseMilliseconds`/`parseSeconds` | 18 | local | 80 |

## 4. Suggested execution order

Batches are ordered mechanical/zero-risk first, behavioural last. Items within
a batch touch different files and may run in parallel unless noted.

- **Batch 1 — dead code (mechanical, zero-risk).** `modules/vision/source/vision/sad.cpp:35`
  and `entry/m0-demo/capture-output.cpp:124` (+ `capture-output.hpp:33-37`).
  Independent files; parallel-safe.
- **Batch 2 — Pattern B reuse (mechanical/local).** m0-demo `dpiDeclarationName`
  (`entry/m0-demo/main.cpp:36`, `entry/m0-demo/capture-mode.cpp:32`, plus the
  `UF_REFLECT_ENUM` addition in `controller/dpi.hpp`) and workbench
  `entry/workbench/app/panels.cpp:79`. **Warning:** `panels.cpp` is in the
  current uncommitted set (see below) — sequence this after or around that work.
- **Batch 3 — Pattern C helpers (local refactor).**
  `modules/controller/source/controller/platform/windows-capture.cpp:1020`,
  `modules/controller/source/controller/input.cpp:357`, and
  `entry/cli/args.cpp:69`. Three independent files; parallel-safe.
- **Batch 4 — Pattern A shared codec (largest, behaviour-adjacent).** Annotation
  TOML dedup across `authoring-document.cpp`, `runtime-manifest.cpp`,
  `detail/canonical-toml.{hpp,cpp}`, and `catalog.{hpp,cpp}`. Do last; it moves
  code across TUs and must keep diagnostics byte-identical. All edits are in the
  same module — treat as one atomic change, do not split across parallel agents.

**In-flight work flag.** `entry/workbench` currently has uncommitted changes in
`entry/workbench/app/panels.cpp`, `entry/workbench/app/panels.hpp`,
`entry/workbench/app/workbench-app.cpp`, `entry/workbench/authoring-edit.cpp`,
`entry/workbench/authoring-edit.hpp`, and
`tests/workbench/test-authoring-edit.cpp`. The Batch 2 edit to `panels.cpp`
will interleave with that work — coordinate or land after those changes commit.

## 5. Method and limits

- **Models.** Two independent reviewers per group: **Claude** and **Codex**
  (`source` on each finding records origin; `both` = independently reported by
  each).
- **Rubric.** Each candidate scored **0-100** by a verifier instructed to
  **refute by default**; anything **< 80 was cut**. Surviving scores here range
  80-85. 60 candidates were considered; 9 kept.
- **Not covered.** This sweep is quality-only. It does **not** find correctness
  bugs (use `/code-review` for that). It excludes everything under `external/`
  (vendored), all non-C++ files, and any group whose candidates all scored
  below the cut — the **core-domain group kept 0 of 10**, so `modules/core` and
  `modules/domain` are effectively unreviewed for actionable simplifications by
  this pass. Line-saving figures are estimates.

---

## 6. Addendum — re-verification of the `core-domain` group (2026-07-25)

Section 3 reported `core-domain` as 0 kept of 10 candidates. That was re-checked
directly against the tree. The group is **not clean**; the original verifier was
right on three rejections, wrong on four, and two real findings were lost to the
score threshold rather than refuted.

**Rejections that hold.**

- `core/types/enum-reflection.hpp` — the finding claimed only two domain mappings
  need it. It has four production includers: `modules/domain/source/domain/error.hpp`,
  `modules/domain/source/domain/frame.hpp`, `modules/engine/source/engine/trace.cpp`,
  `entry/cli/run.cpp`.
- `core/utility/scope-exit.hpp` — the finding proposed replacing it with
  `std::scope_exit`. **No such facility exists in C++23**; scope guards remain
  `std::experimental::scope_exit` in Library Fundamentals TS v3. False premise.
- `domain/detection.hpp` 750 ms clamp — documented shorten-only safety fuse with
  production users. Behavioural change, correctly refused.

**Rejections that do not hold.** `core/types/flags.hpp`,
`core/concurrency/synchronized.hpp`, `core/control/control-flow.hpp`,
`core/types/non-zero.hpp` were all rejected on the grounds that
`core-reuse.md` documents them as intentional kernel facilities. That file's
list is an **inventory** ("The core capability kernel currently provides:"),
not a design justification, and `core-reuse.md` itself directs this exact
question to the `evaluate-core-capability` skill. Measured: each of the four is
included by exactly one file, `tests/core/test-capabilities.cpp` — **zero
production includers**.

The original proposal (delete them) is equally wrong: `CLAUDE.md` defines this
repository as a reusable C++23 project foundation, where pre-provided vocabulary
types with no caller yet are defensible. Correct disposition: treat all four as
**unvalidated core surface** and run them through `evaluate-core-capability`.
That is a governance question, out of scope for a simplification sweep.

**Real findings lost to the `<80` cut.**

- `modules/domain/source/domain/space.hpp:188` (scored 75) —
  `static constexpr auto k_maxExactFrameDimension = k_maxExactFrameDimension;`
  is a self-referential class-scope shadow of the namespace constant at
  `space.hpp:108`. It compiles today under MSVC, but whether the initializer
  binds the member or the namespace constant should be confirmed by a compiler
  rather than assumed — `space.cpp:170-171` (`CoordinateTransform::create`) is
  the only site the shadow can reach. Delete line 188. This is closer to a
  latent defect than a style cleanup, which is why a simplification-only
  mandate under-weighted it.
- `domain/space.hpp:276` `PixelRectHash` (scored 50) — confirmed used only by
  `tests/domain/test-space.cpp:413`, but it is a documented domain API in
  `docs/knowledge/*/module-domain.md`. Genuine judgement call; 50 is fair.

**Method limitation this exposed.** Codex returned only 1 finding for this group
against Claude's 9, so `core-domain` was effectively a single-model review. The
thin result for this group is better explained by that lost coverage than by
verifier strictness.
