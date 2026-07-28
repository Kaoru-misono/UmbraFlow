# Full-project architecture and abstraction review — 2026-07-27

> Status: **Implementation follow-up complete (2026-07-28)**. The original
> review was report-only. The 2026-07-28 follow-up re-verified each actionable
> claim against the rebased tree before implementation; corrections are
> recorded inline rather than silently rewriting the historical finding. This review
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

**Follow-up result (2026-07-28):** all still-valid correctness, medium, and
low-risk findings outside the frozen `m0-demo` reference were implemented.
Items whose complete repair would modify that frozen target are explicitly
deferred below; incorrect or stale claims retain their corrected disposition.

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

> **DEFERRED (2026-07-28):** a complete `InputSession` migration includes the
> frozen `m0-demo` caller. Keeping the free functions as compatibility wrappers
> would create two public action surfaces without delivering the proposed
> pairing guarantee. Revisit when `m0-demo` is unfrozen or retired.

### 2.2 m0-demo loop drivers (low, frozen)

`entry/m0-demo/pipeline.cpp` — `runOne`, `runLoopSteps`, `clickWhenPresent`,
`waitUntilPresent` et al. thread the identical 5-6 argument context
`(Machine&, Templates const&, LoopConfig const&, loopIndex, ClickPacer&,
JsonlLog&)`. Textbook `LoopDriver` shape, but m0-demo is the frozen M0
acceptance reference — recorded for a future un-freeze only.

> **DEFERRED:** unchanged by design under the repository's frozen-reference
> rule.

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

### 3.1 `EngineSession` movable while `Observation` stores a raw back-pointer (medium) — RESOLVED (2026-07)

~~`modules/engine/source/engine/session.hpp:179` defaults the move constructor;
`Observation` holds `EngineSession* m_session` (session.hpp:92). Moving a
session after it has vended an observation leaves the handle's back-pointer
dangling; `Observation::resolvePage`/`findAction` (session.cpp:162, 177) then
dereference the moved-from session (UB). `act` happens to fail closed via its
identity check; the other two paths do not. Currently unexploited (sessions
are held by stable reference), but the header's lifetime argument is stronger
than what the type enforces.~~

> **Re-evaluated 2026-07-28:** the dependency inversion remains the correct
> structural repair, but the proposed `CaptureSessionId` replacement is invalid.
> `CaptureSessionId` identifies a capture session and is one component of frame
> identity; `EngineSession` neither owns nor has such an id. Reusing it as an
> engine-session identity would merge two concepts that `CONTEXT.md` now
> distinguishes explicitly. The original churn estimate was also incomplete:
> there are four production call sites and 22 test call sites.

**Agreed resolution direction (2026-07-28):** move `resolvePage` and
`findAction` onto `EngineSession`, matching `act`, and delete the raw
back-pointer so `Observation` is a pure move-only value handle. Preserve the
foreign-session invariant with a private, shared immutable identity token:
`EngineSession` and each observation it vends share the token and operations
compare token identity without retaining or dereferencing the session. The
token follows a moved session, so existing observations remain associated with
the destination and no pointer into the moved object exists.

This internal token is necessary because the existing authorization value
fence is not, by itself, a session fence: its delivery identity is constructed
from the incoming observation, so every compared `(CaptureSessionId,
TargetGeneration, FrameId)` value can agree even when a different
`EngineSession` receives the handle. The token preserves the existing
`InternalInvariant` rejection for that programming error without assigning new
meaning to the capture `CaptureSessionId` or exposing a public engine-run identity.
This removes the dangling class of bug by construction and keeps `create()`
returning a movable value.

> **RESOLVED (2026-07-28):** `Observation` now shares only the private immutable
> identity token; `resolvePage` and `findAction` are `EngineSession` operations.
> Tests cover session move after vending an observation and rejection by a
> foreign session.

**Naming disposition (2026-07-28):** retain `EngineSession`. It names the
stateful engine capability scope that owns the runtime and three ports;
`CaptureSessionId` names capture-session frame identity. `EngineRun` would
instead conflate this capability object with the separate `EngineRunId`
execution vocabulary. The distinction is now recorded in `CONTEXT.md`.

> **Naming amendment (2026-07-28):** the capture identity type was renamed
> from the generic `SessionId` spelling to `CaptureSessionId`. Existing
> `sessionId` members and serialized trace fields retain their spelling because
> their enclosing capture/frame context already fixes the meaning and changing
> the wire key would be a schema break.

Alternatives considered and rejected: a vend-flag `UF_CHECK` in the move
constructor (runtime patch over a structural problem), heap-pinning via
`unique_ptr` return (protects the fragile premise instead of deleting it), and
reusing `CaptureSessionId` (capture identity, not engine identity). A public
`EngineRunId` is also unnecessary: the stable token is private mechanism for an
already-existing API invariant, not new domain vocabulary.

### 3.2 `annotation/catalog.hpp` is a de-facto module prelude (medium) — RESOLVED (2026-07)

~~Named for `RecognitionCatalog` (line 259) but also defines the module's
entire id/geometry vocabulary (`ResourceId`, `ProjectId`, `ResourceName`,
`AnnotationType`, `ProjectFingerprint`, `SimilarityThreshold`,
`TemplateOffset`, spec types). Nearly every other header includes it only for
those types. Recommendation: split the identifier vocabulary into an
`identifiers.hpp` (or `resource.hpp`); leave `catalog.hpp` owning only the
catalog and recognizer/page definitions.~~

> **RESOLVED (2026-07-28):** shared resource vocabulary now lives in
> `annotation/resource.hpp`; `catalog.hpp` retains recognizer/page/catalog
> definitions, and vocabulary-only consumers include the narrower header.

### 3.3 Product identity constant inside the `core` leaf (medium) — RESOLVED (2026-07)

~~`modules/core/source/core/project.hpp:7` —
`k_projectName = "UmbraFlow"` is product identity inside the
mechanism-only leaf that ARCHITECTURE.md reserves for mechanism
(consumed by `entry/cli/main.cpp:68`). Move to an entry-level constant or
`domain`. Already noted as an outlier in `docs/knowledge/en/module-core.md`.~~

> **RESOLVED (2026-07-28):** the product-name constant is local to the CLI
> entry; `core/project.hpp` was removed.
>
> **Amended 2026-07-28:** the repository-root application manifest now owns the
> application name and version. CMake generates a typed `application-info.hpp` used
> privately by the CLI executable; the value still does not enter `core`.

### 3.4 Workbench `app/` mixes model and view layers (low-medium) — RESOLVED (2026-07)

~~`app/workbench-app.*` (platform-free document/undo/selection core) and
`app/canvas-math.*` (pure geometry) sit beside `panels.cpp`/`main.cpp`
(ImGui + shell). Top-level logic headers include *up* into `app/`
(`panel-state.hpp:5-6`, `authoring-actions.hpp:5`, `model-check-view.hpp:5`),
so `app/` is not the top of the stack. Move the platform-free pair to the top
level (or a `core/` subdir) for boundary legibility.~~

> **RESOLVED (2026-07-28):** `workbench-app.*` and `canvas-math.*` now live at
> `entry/workbench/`; `app/` contains the ImGui composition/view layer.

### 3.5 Misleading filenames in controller (low) — RESOLVED (2026-07)

- ~~`detail/capture-d3d.hpp` contains zero D3D — it is `ClientCropRect` pure
  crop geometry plus `readbackBgra8` row-unpadding. Rename to
  `capture-crop`/`capture-readback`.~~
- ~~`input-guard.cpp` / `detail/input-guard.hpp` contain no guard — both are
  entirely `AuditLog`/`AuditLogAccess`. Rename to `audit-log.*`.~~

> **RESOLVED (2026-07-28):** the files and their focused tests are now named
> `capture-readback.*` / `test-capture-readback.cpp` and
> `audit-log.*` / `audit-log-access.hpp` / `test-audit-log.cpp`.

### 3.6 Engine ports omit the mandated interface prefix (low) — RESOLVED (2026-07)

~~`modules/engine/source/engine/ports.hpp:22,48,71` — `FrameSource`,
`ActionSink`, `TraceSink` are pure-virtual interfaces; the coding standard
says `IPascalCase`. No first-party `I`-prefixed class exists anywhere, so the
deviation is consistent. Either rename or amend the standard via
`correct-doc-drift` so rule and code agree.~~

> **RESOLVED (2026-07):** commit `1615197` renamed the three engine ports to
> `IFrameSource`, `IActionSink`, and `ITraceSink` and made the repository-wide
> `ITypeName` interface rule explicit. The follow-up also covers the task
> module's independently discovered pure interface, now `ITaskTraceSink`.

## 4. Other findings

### Medium

1. **§3.1 above** (the only correctness finding).
2. **`entry/cli/run.cpp:14` — `formatRunError` contract drift. — RESOLVED (2026-07)**
   ~~The header
   contract (`run.hpp:57`) promises the rendered line includes the
   originating source location; the implementation never appends it
   (m0-demo's `formatAutomationError` does). Append `error.location()` or fix
   the comment.~~
   *Rendering now appends the source basename and
   line, with a focused CLI test.*
3. **Workbench: placement-withdrawal closure rule implemented three times. — RESOLVED (2026-07)**
   ~~`edit-page.cpp:835` (canonical), `panels.cpp:2010`, `panels.cpp:2848` —
   the third is ~200 lines of authoring mutation living inside the draw
   function `drawPageMembership`. Extract one action-layer function (e.g.
   `withdrawFromPage`/`setPageRole`) and call it from both panel sites.~~
   *`removePlacementFromPage` is the single edit-layer
   operation used by `EditPage` and both panel paths; tests pin the last-action
   and page-anchor closure rules.*

### Low (actionable)

- **Detection label borrow — RESOLVED (2026-07).**
  ~~`domain/detection.cpp:39` — `Detection::label()` returns the owning
  `std::string` by value on every call; return `Label const&` with
  `UF_LIFETIME_BOUND`.~~
  *The accessor now returns a lifetime-bound `Label const&`.*
- **Workbench draft snapshot — RESOLVED (2026-07).**
  ~~Workbench project tree deep-copies the whole draft per row per frame
  (`panels.cpp:760`, `1533`) despite an existing snapshot at `panels.cpp:1213`;
  reuse the captured local.~~
  *All membership rows use the one frame-local draft snapshot.*
- **Texture cache eviction — RESOLVED (2026-07).**
  ~~`windows-texture-cache.cpp:59-165` — `TextureCache` never evicts; deleted
  sources leak GPU textures for the shell's lifetime. Add a prune hook
  mirroring `AppState::pruneSourceCacheToDocument`.~~
  *The panel service forwards the live source closure to
  `TextureCache::pruneTo` after edits/imports.*
- **Shared UTF-8 decoder — RESOLVED (2026-07).**
  ~~`controller/input.cpp:33` — hand-rolled UTF-8 validator/decoder duplicates
  security-sensitive logic that belongs beside `core/text/utf8`.~~
  *`core::decodeUtf8Scalars` shares the validator's state machine; controller
  only performs UTF-16 encoding.*
- **Shared annotation field codecs — RESOLVED (2026-07).**
  ~~Annotation id-parsing (`runtime-manifest.cpp:49-77` vs
  `authoring-document.cpp:201-215`) and fingerprint codec
  (`authoring-document.cpp:423-446` vs `runtime-manifest.cpp:336-350`)
  duplicated across TUs; hoist into `detail/` (overlaps the 2026-07-25
  simplify-sweep Pattern A).~~
  *Generic ID parsers and fingerprint parse/serialization helpers live in
  `detail/annotation-fields.*`.*
- `clientOriginDesktop` implemented three times
  (`entry/cli/platform/windows-target-geometry.cpp:24`,
  `entry/m0-demo/platform/windows-guard.cpp:168`,
  `entry/workbench/platform/windows-capture-source.cpp:74`).
  **DEFERRED (2026-07-28):** complete consolidation would modify the frozen
  `m0-demo` reference; a partial two-entry helper would leave the duplication
  and create two authorities.
- `currentIoError()` was reported as byte-identical in three TUs
  (`entry/cli/file-trace-sink.cpp:20`, `entry/m0-demo/log-jsonl.cpp:37`,
  `entry/m0-demo/input-agent.cpp:58`).
  **CORRECTED / DEFERRED (2026-07-28):** there are five copies, also in
  `modules/task/file-trace-sink.cpp` and `modules/image/ffi/png-encoder.cpp`.
  Complete consolidation crosses the frozen target; partial consolidation
  would preserve competing implementations.
- Dead code: `uf::m0_demo::clientOriginDesktop` forwarder
  (`entry/m0-demo/guard.hpp:83`) has no callers.
  **DEFERRED (2026-07-28):** confirmed dead but retained with the frozen
  reference.
- `controller/input.hpp:174,203` — `AuditRecord::at` and
  `DeliveryIdentity::sessionId` lack in-class initializers while sibling
  members carry `{}`; verify default-constructibility and make consistent.
  **Re-evaluated 2026-07-28:** no change is correct. Both strong domain values
  deliberately lack a default state and every construction site supplies them;
  adding `{}` would not compile and inventing a sentinel would violate the
  valid-states rule.
- **Sandbox GC surface — RESOLVED (2026-07).**
  ~~`script/ffi/sandbox.cpp` — `math.randomseed` (inert) and `collectgarbage`
  (minor nondeterminism surface) survive the sandbox; fold into the D9
  clock/RNG wave.~~ *Re-evaluated 2026-07-28: this claim was already stale when
  written: `math.randomseed` is removed and this Luau build never registers
  `collectgarbage`. The follow-up instead removes the real residual GC
  observation surface, `gcinfo`. `installSandbox` now explicitly nils it and
  the sandbox test suite pins its absence.*
- **Direct `ContentHash` include — RESOLVED (2026-07).**
  ~~`annotation/recognition-runtime.hpp:24,66` names `ContentHash` but reaches
  `content-hash.hpp` only transitively; add the direct include.~~
  *The header now includes `content-hash.hpp` directly.*
- **Luau VM ownership — RESOLVED (2026-07).**
  ~~`script/ffi/engine.cpp:39` — `Engine::Impl::m_state` is a public member
  carrying the private `m_` prefix, and an owning raw pointer whose ownership
  note lives only in the destructor; rename/annotate.~~
  *Private `Impl` state now uses
  `unique_ptr<lua_State, LuaStateDeleter>`; declaration order keeps the quota
  ledger alive through VM teardown.*
- **Engine trailing returns — RESOLVED (2026-07).**
  ~~Engine leading-`void` signatures (`session.cpp:58,579`,
  `trace.cpp:128-150`) deviate from the trailing-return-type rule; fix
  opportunistically.~~
  *All six declarations/definitions use trailing return types.*

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
