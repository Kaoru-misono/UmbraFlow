# Full-project architecture and abstraction review — 2026-07-27

> Status: **Report-only (2026-07-27)**. No C++ source was changed. This review
> covered every first-party `.cpp`/`.hpp` under `modules/` and `entry/`
> (vendored `external/` excluded, ~230 files), split across six independent
> review passes (core+domain, annotation, controller+vision+image,
> engine+script working tree, cli+m0-demo, workbench). Every finding below was
> verified by re-reading the cited lines. Focus areas requested: free-function
> clusters that should be classes, and overall architecture clarity.

## 1. Summary

No high-severity bug was found. The codebase closely matches
`docs/ARCHITECTURE.md`: the module graph is acyclic, platform types stay
behind `platform/`/`ffi/` boundaries with accurate `// SAFETY:` comments, OS
and COM handles are uniformly RAII-wrapped, and ownership is visible in
signatures throughout. The requested "should-be-a-class" concern is largely a
non-issue: nearly every free-function layer is a deliberate stateless design
whose invariants are already guarded by an adjacent class. One genuine
consolidation candidate exists (`InputSession`), one medium lifetime hole
(`EngineSession` move vs `Observation` back-pointer), and a handful of
clarity/duplication items.

Priority order for follow-up:

1. `EngineSession` move / `Observation` back-pointer (the only correctness
   finding) — resolution direction agreed, see §3.1.
2. Workbench triple-duplicated placement-withdrawal rule (§4.1).
3. `InputSession` consolidation in controller (§2.1).
4. `annotation/catalog.hpp` vocabulary split and `core/project.hpp`
   relocation (§3).

## 2. Free functions that should be classes

### 2.1 controller input actions → `InputSession` (medium-low)

`modules/controller/source/controller/input.hpp:256-353` — 11 input actions
(`movePointer`, `click`, `pointerDown/Up`, `longPress`, `keyPress`,
`keyDown/Up`, `inputText`, `inputUnichar`, `releaseHeld`) all repeat the tail
parameters `(DeliveryTarget, HeldInputs&, AuditLog&)`. `HeldInputs` and
`AuditLog` always travel together and jointly form the mutable state of one
input-delivery session. Recommendation: an `InputSession { HeldInputs;
AuditLog; }` with the actions as members taking only
`(DeliveryTarget, lease, point/key)`. Ergonomics and pairing guarantee; the
existing runtime guards are not an invariant hole.

### 2.2 m0-demo loop drivers (low, frozen)

`entry/m0-demo/pipeline.cpp` — `runOne`, `runLoopSteps`, `clickWhenPresent`,
`waitUntilPresent` et al. thread the identical 5-6 argument context
`(Machine&, Templates const&, LoopConfig const&, loopIndex, ClickPacer&,
JsonlLog&)`. Textbook `LoopDriver` shape, but m0-demo is the frozen M0
acceptance reference — recorded for a future un-freeze only.

### 2.3 Evaluated and correctly left as free functions

Each of these was examined and the free-function form is the right one:

- annotation serialize/parse helper families — the stateful half is already
  the `CanonicalTomlReader` class; the append side is a pure builder layer.
- `script/ffi/sandbox.cpp` (`installSandbox`, `deepFreeze`,
  `runNumberOnThread`) — stateless FFI layer over a VM owned by
  `Engine::Impl`; a class would break the Luau-free test seam in
  `sandbox-probe`.
- workbench `authoring-actions` — invariants already guarded by `AppState`
  (undo/dirty/selection) and `EditPage` (one editor, one commit); a class
  would only add stored borrows.
- `entry/cli` composition root — single linear `runProduct` plus stateless
  pure transforms.
- domain/core conversion and classifier functions — stateless by design; the
  stateful parts (`CoordinateTransform`, leases) are already classes.

## 3. Architecture clarity

Positive baseline: platform isolation is genuinely clean (no `HWND`/`HRESULT`/
`ID3D11*`/`lua_State` escapes its owning boundary), the controller `detail/`
pure-logic split is the strongest structural asset, and `image/ffi` returns
owning abstractions with per-cast SAFETY justification.

### 3.1 `EngineSession` movable while `Observation` stores a raw back-pointer (medium)

`modules/engine/source/engine/session.hpp:179` defaults the move constructor;
`Observation` holds `EngineSession* m_session` (session.hpp:92). Moving a
session after it has vended an observation leaves the handle's back-pointer
dangling; `Observation::resolvePage`/`findAction` (session.cpp:162, 177) then
dereference the moved-from session (UB). `act` happens to fail closed via its
identity check; the other two paths do not. Currently unexploited (sessions
are held by stable reference), but the header's lifetime argument is stronger
than what the type enforces.

**Agreed resolution direction (not yet implemented):** invert the dependency
instead of patching the move. `act` already has the correct shape
(`session.act(std::move(obs), ...)`); move `resolvePage`/`findAction` onto
`EngineSession` the same way, delete the back-pointer so `Observation`
becomes a pure value handle, and replace address identity with value identity
via the `SessionId` already carried in the lease/frame identity (the domain's
existing value-fencing pattern). This removes the dangling class of bug by
construction, keeps `create()` returning by value with a defaulted move, and
extends the foreign-session fail-closed check to all three operations.
Call-site churn is four sites: `entry/cli/run-windows.cpp:189`,
`session.cpp:548`, and two in `modules/task/source/task/task-context.cpp`.
Alternatives considered and rejected: a vend-flag `UF_CHECK` in the move
constructor (runtime patch over a structural problem) and heap-pinning via
`unique_ptr` return (protects the fragile premise instead of deleting it).

### 3.2 `annotation/catalog.hpp` is a de-facto module prelude (medium)

Named for `RecognitionCatalog` (line 259) but also defines the module's
entire id/geometry vocabulary (`ResourceId`, `ProjectId`, `ResourceName`,
`AnnotationType`, `ProjectFingerprint`, `SimilarityThreshold`,
`TemplateOffset`, spec types). Nearly every other header includes it only for
those types. Recommendation: split the identifier vocabulary into an
`identifiers.hpp` (or `resource.hpp`); leave `catalog.hpp` owning only the
catalog and recognizer/page definitions.

### 3.3 Product identity constant inside the `core` leaf (medium)

`modules/core/source/core/project.hpp:7` —
`k_projectName = "UmbraFlow"` is product identity inside the
mechanism-only leaf that ARCHITECTURE.md reserves for mechanism
(consumed by `entry/cli/main.cpp:68`). Move to an entry-level constant or
`domain`. Already noted as an outlier in `docs/knowledge/en/module-core.md`.

### 3.4 Workbench `app/` mixes model and view layers (low-medium)

`app/workbench-app.*` (platform-free document/undo/selection core) and
`app/canvas-math.*` (pure geometry) sit beside `panels.cpp`/`main.cpp`
(ImGui + shell). Top-level logic headers include *up* into `app/`
(`panel-state.hpp:5-6`, `authoring-actions.hpp:5`, `model-check-view.hpp:5`),
so `app/` is not the top of the stack. Move the platform-free pair to the top
level (or a `core/` subdir) for boundary legibility.

### 3.5 Misleading filenames in controller (low)

- `detail/capture-d3d.hpp` contains zero D3D — it is `ClientCropRect` pure
  crop geometry plus `readbackBgra8` row-unpadding. Rename to
  `capture-crop`/`capture-readback`.
- `input-guard.cpp` / `detail/input-guard.hpp` contain no guard — both are
  entirely `AuditLog`/`AuditLogAccess`. Rename to `audit-log.*`.

### 3.6 Engine ports omit the mandated interface prefix (low)

`modules/engine/source/engine/ports.hpp:22,48,71` — `FrameSource`,
`ActionSink`, `TraceSink` are pure-virtual interfaces; the coding standard
says `IPascalCase`. No first-party `I`-prefixed class exists anywhere, so the
deviation is consistent. Either rename or amend the standard via
`correct-doc-drift` so rule and code agree.

## 4. Other findings

### Medium

1. **§3.1 above** (the only correctness finding).
2. **`entry/cli/run.cpp:14` — `formatRunError` contract drift.** The header
   contract (`run.hpp:57`) promises the rendered line includes the
   originating source location; the implementation never appends it
   (m0-demo's `formatAutomationError` does). Append `error.location()` or fix
   the comment.
3. **Workbench: placement-withdrawal closure rule implemented three times.**
   `edit-page.cpp:835` (canonical), `panels.cpp:2010`, `panels.cpp:2848` —
   the third is ~200 lines of authoring mutation living inside the draw
   function `drawPageMembership`. Extract one action-layer function (e.g.
   `withdrawFromPage`/`setPageRole`) and call it from both panel sites.

### Low (actionable)

- `domain/detection.cpp:39` — `Detection::label()` returns the owning
  `std::string` by value on every call; return `Label const&` with
  `UF_LIFETIME_BOUND`.
- Workbench project tree deep-copies the whole draft per row per frame
  (`panels.cpp:760`, `1533`) despite an existing snapshot at `panels.cpp:1213`;
  reuse the captured local.
- `windows-texture-cache.cpp:59-165` — `TextureCache` never evicts; deleted
  sources leak GPU textures for the shell's lifetime. Add a prune hook
  mirroring `AppState::pruneSourceCacheToDocument`.
- `controller/input.cpp:33` — hand-rolled UTF-8 validator/decoder duplicates
  security-sensitive logic that belongs beside `core/text/utf8`.
- annotation: id-parsing (`runtime-manifest.cpp:49-77` vs
  `authoring-document.cpp:201-215`) and fingerprint codec
  (`authoring-document.cpp:423-446` vs `runtime-manifest.cpp:336-350`)
  duplicated across TUs; hoist into `detail/` (overlaps the 2026-07-25
  simplify-sweep Pattern A).
- `clientOriginDesktop` implemented three times
  (`entry/cli/platform/windows-target-geometry.cpp:24`,
  `entry/m0-demo/platform/windows-guard.cpp:168`,
  `entry/workbench/platform/windows-capture-source.cpp:74`).
- `currentIoError()` byte-identical in three TUs
  (`entry/cli/file-trace-sink.cpp:20`, `entry/m0-demo/log-jsonl.cpp:37`,
  `entry/m0-demo/input-agent.cpp:58`).
- Dead code: `uf::m0_demo::clientOriginDesktop` forwarder
  (`entry/m0-demo/guard.hpp:83`) has no callers.
- `controller/input.hpp:174,203` — `AuditRecord::at` and
  `DeliveryIdentity::sessionId` lack in-class initializers while sibling
  members carry `{}`; verify default-constructibility and make consistent.
- `script/ffi/sandbox.cpp` — `math.randomseed` (inert) and `collectgarbage`
  (minor nondeterminism surface) survive the sandbox; fold into the D9
  clock/RNG wave.
- `annotation/recognition-runtime.hpp:24,66` — names `ContentHash` but reaches
  `content-hash.hpp` only transitively; add the direct include.
- `script/ffi/engine.cpp:39` — `Engine::Impl::m_state` is a public member
  carrying the private `m_` prefix, and an owning raw pointer whose ownership
  note lives only in the destructor; rename/annotate.
- Engine: leading-`void` signatures (`session.cpp:58,579`,
  `trace.cpp:128-150`) deviate from the trailing-return-type rule; fix
  opportunistically.

## 5. Verified strengths (no action)

- WGC callbacks capture `shared_ptr<FrameSlot>` by value with documented
  non-inverting lock ordering; D3D swapchain resize release sequence correct
  (`windows-gui-shell.cpp:525-558`); `ModelCheckJob` copy-everything +
  stop-token threading model correct.
- Persistence atomicity: temp-write + `MoveFileExW` + `scopeExit` cleanup +
  SHA-256 re-verification on load (`project-persistence.cpp:170-266`).
- Hand-written SHA-256, checked-cast/checked-multiply boundary cases, and
  lease/generation invariants all correct under re-reading.
- Engine/script test coverage is thorough across the fail-closed edges
  (expired lease, double delivery, foreign session, mid-recognition cancel,
  frozen-table rejection, per-thread isolation). Known gap: no
  infinite-loop/quota test until the interrupt wave lands
  (`TODO(cpp-debt)` at `script/ffi/engine.cpp:44-47`).
- Workbench snapshot-before-edit discipline defuses the use-after-free class
  recorded in `docs/pitfalls/workbench-authoring-ui.md`; `EditPage` handles
  are non-copyable/non-movable and re-resolve ids per call.
