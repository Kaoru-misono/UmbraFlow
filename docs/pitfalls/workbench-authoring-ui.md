# Workbench authoring UI

> **HISTORICAL (2026-07-31): every prescription in this file is unreachable.**
> The `umbra-workbench` GUI was archived in `b57b67b` — `entry/workbench/app/`
> (`main.cpp`, `panels.*`), the Dear ImGui + D3D11 shell, the file dialog, the
> capture source, the imgui submodule, and `tests/workbench/asan-smoke-fixture.cpp`
> with them. There is no `umbra-workbench.exe`, no `--smoke` flag, and no
> `AsanSmoke` CTest label (the `x64-asan` preset in `CMakePresets.json` outlived
> the tests that used it). `retypeRecognizer`, `addDefaultElement`, and
> `setSelectedElementId` were deleted. The catalog rules the second entry
> deadlocks over are gone too: `AnnotationType` is a capability set, and
> `allowed_page_ids` no longer exists — a page's `PageReference` exercising
> `interact` IS the authorization. Deciding artifact:
> [the capability plan](../plans/2026-07-31-annotation-model-capabilities.md)
> §四之二.1 (GUI retirement) and §2.2 (the model). Nothing is deleted here; the
> entries stay because the general rules they distilled outlive the panels.
>
> **Amended 2026-08-01 (`a80ea07`).** The live pointers this banner first gave
> were themselves retired within the week: `modules/annotation` and every
> authoring binary went with the C++ page model, so `authoring-edit.*`,
> `edit-page.*`, `umbra-authoring`, `EditPage`, `AuthoringDraft`,
> `AuthoringEditHistory`, `applyCommittedPage`, `ElementCapabilities`,
> `ExercisedCapabilities` and `RecognitionCatalog::create` no longer exist
> (`docs/ARCHITECTURE.md`). Each rule below is re-aimed at the Luau model that
> holds it now.
>
> **What still transfers, and to what:**
>
> - *Do not commit while holding a borrow into the document.* The C++ form was
>   `AuthoringEditHistory::apply` swapping the whole document out from under every
>   live borrow. The Luau model answers the same rule by construction: writing a
>   row or an appearance REBUILDS the page rather than mutating it
>   (`scribe.add_reference`, `scribe.add_appearance`,
>   `modules/task/runtime/scribe.luau`), so a page an agent is holding can already
>   be one the model no longer has. That is why an edge names its ends and a write
>   names its target instead of holding either: a name always resolves to the page
>   the model does have.
>
>   > **Mechanism history.** The *Fix* section below prescribes parking a
>   > `PendingEdit` on `PanelUiState` and letting `drawWorkbench` apply it;
>   > `f768e6c` then replaced that with an `EditPage` owning its draft by value.
>   > Both shapes are deleted. Read them as history — the rule they distil is
>   > unchanged.
> - *Cross-field domain invariants can deadlock per-field editing.* The specific
>   deadlock is gone with `allowed_page_ids`, but the shape recurs anywhere a
>   single-field editor meets a multi-field invariant. The surviving lower bound
>   of that kind is `model.CapabilitySet`: a capability list that names nothing is
>   refused, because an element nothing can identify, click or read is an entry
>   nobody can explain (`modules/task/runtime/model.luau`).
> - *A fixed default name collides on the second create* has no analogue left:
>   `Element.new` and `Page.new` both refuse an empty name, so there is no default
>   to collide. The uniqueness rule NARROWED with them — elements and pages are
>   two registries (`element_by_name` / `page_by_name`, checked by
>   `scribe.add_element` and `scribe.add_page`), so a name is unique within its
>   kind and an element may now share one with a page. Anything that reads a bare
>   name as unambiguous across both kinds is relying on a convention nothing
>   enforces.
> - *An entity with no list panel is unreachable after creation* has no analogue —
>   a loaded model enumerates everything it holds — and is kept as history only.

Failure knowledge for the `umbra-workbench` panels — the immediate-mode layer
between the author and the authoring document. Recorded on 2026-07-25 during the
first full manual GUI acceptance, which is what surfaced all of it; none of these
were reachable from the programmatic backend driver used for the A1+B1 closed
loop.

## An immediate-mode panel must not commit while it holds a borrow into the document

### Symptom

None observed — this is latent undefined behaviour, which is why it survived a
green test suite, four static gates, and two multi-dimensional reviews. It
surfaces as a reader, not a crash: values read after the commit come from freed
storage and happen to still look right.

### Root cause

`drawPropertiesPanel` opened with

```cpp
auto const* definition = state.document().catalog().findElement(*elementId);
```

and then used `definition` for the whole panel: the type combo, the threshold
field, the click-offset widgets, and finally `drawPageMembership(state, ui, *definition)`.
But every accepted edit inside that panel ran `AuthoringEditHistory::apply`,
whose last act is `m_current = std::move(next)` — the entire document is
replaced, so `definition` dangles from the first successful commit onward.
`drawPageMembership` had the same shape one level down: it iterated the
`std::span` returned by `catalog().pages()` and committed inside the loop, so the
span and the `PageSignature` it was reading were freed mid-iteration.

The general rule this violates: a panel that borrows into a value owns nothing,
and committing swaps the value out from under every live borrow in the frame.
Any per-widget commit in a panel that also reads borrowed state is this bug.

### Fix

Defer the commit. Panels call `requestEdit(ui, draft, description)`, which parks
one `PendingEdit` on `PanelUiState`; `drawWorkbench` applies it once, after the
panels that borrow into the document have finished drawing and before
`drawActionsPanel`, which mutates the document itself and re-reads everything it
touches. Ordering matters in both directions: applying earlier would reintroduce
the dangling borrow, and applying after the actions panel would let an undo race
the rename that preceded it in the same frame.

One request per frame is enough — only a widget deactivating in the same frame as
another widget's click can produce two, and the second is built against the same
document as the first, so accepting both would silently drop one. The first
request wins and the second click is retried by the author on the next frame.

### Regression check

No unit test pins the frame-lifetime rule — the panels have no headless seam.
`umbra-workbench --smoke N` exercises N frames of the whole draw path; run it
under a debug allocator or ASan to make a regression observable. The structural
check is a review one: any new `state.applyEdit` reachable from a panel that
holds a pointer or span into `state.document()` is the same bug.

**Implemented 2026-07-26.** The ASan smoke is now automated as two CTest tests
under the `AsanSmoke` label (registered in `tests/CMakeLists.txt`, kept off the
`CI` label so the normal tree is undisturbed):

- `asan-smoke-fixture` — a skipped doctest case in `test-workbench`
  (`tests/workbench/asan-smoke-fixture.cpp`) generates a valid one-source,
  one-element, one-page project into the build tree
  (`build/x64-asan/tests/asan-smoke-project`). It is generated rather than
  committed because the loader verifies each source PNG's SHA-256 and decoded
  dimensions, so the fixture cannot be hand-authored, and a committed binary
  would silently rot if the authoring TOML format changed. It does **not** rely
  on the gitignored `tests/assets/real-regression/`.
- `workbench-asan-smoke` — launches the real `umbra-workbench.exe` against that
  project with `--smoke 180` (≈2 s wall, well under the 120 s timeout). ASan
  aborts with a non-zero exit on any error, which fails the test.

The `x64-asan` preset builds `umbra-workbench.exe` under MSVC ASan. Two flag
fixes were required and live in `CMakePresets.json`: `CMAKE_CXX_FLAGS_DEBUG` is
overridden to drop `/RTC1` (the compiler rejects `/RTC1` together with
`/fsanitize=address`, error D8016), and the debug linker flags force
`/INCREMENTAL:NO` (ASan is incompatible with incremental linking). The Dear
ImGui backend library is not instrumented (it carries no safety profile), but
every first-party panel/draw source in the executable is.

Invocation:

```bash
# Windows: activate MSVC first (.claude/skills/build-project/script/windows/build-env.bat)
cmake --preset x64-asan
cmake --build --preset x64-asan
ctest --test-dir build/x64-asan -L AsanSmoke --output-on-failure
```

**Pending:** a CI job that configures/builds `x64-asan` and runs
`ctest -L AsanSmoke` is not yet wired into `.github/workflows/ci.yml` (GitHub
Actions billing is currently blocked, and the job needs a Windows runner that
can create the GUI window the smoke opens). Add it once billing is restored.

## Cross-field domain invariants can deadlock per-widget editing

### Symptom

Two rejections alternating forever, with no ordering that clears both:

```
edit rejected: action_target element must authorize at least one page
edit rejected: page_anchor membership must be expressed by page signatures
```

Changing an element's type to `ActionTarget` is refused because it authorizes
no page; authorizing a page first is refused because a page anchor may not hold
authorizations. The workbench could not author an action target at all.

### Root cause

The catalog ties four rules to the annotation type
(`modules/annotation/source/annotation/catalog.cpp`):

- a page anchor's `allowed_page_ids` must be empty
- an action target's `allowed_page_ids` must be non-empty
- only an action target may carry a default click
- a page signature may name only page anchors

The properties panel committed one widget per edit, so the type and the fields
the catalog ties to it could never move together. This is not a validation bug —
each rule is correct on its own, and each is reachable by a legal document. The
bug is an editor that can only express one field at a time against a domain whose
states are defined by several.

### Fix

Make the change a transaction. `retypeRecognizer`
(`entry/workbench/authoring-edit.cpp`) rewrites the type and every dependent
field in one draft, and refuses only where no repair exists: becoming an action
target in a project with no page, or leaving the page-anchor type while being the
only element some page names. Where it must invent state — an action target
needs one authorized page — it prefers the page the element already anchored
over an arbitrary one and reports what it did on the status line, because a
permission the author did not ask for must never be silent. The reverse
direction is lossy (the click offset cannot survive), so that is reported too.

Widgets that can only fail for the current type are disabled with a tooltip
rather than left to produce a rejection.

### Regression check

`test-workbench` covers each conversion, both refusals, the "authorize the page
it anchored" choice, and the reported repairs. The broader lesson has no test: if
a second pair of fields ever deadlocks this way, the per-widget commit model
should be replaced with a pending draft the author applies explicitly, rather
than growing a second transaction helper.

## A fixed default name collides on the second create

### Symptom

`New Element` (or `New Page`) fails with
`edit rejected: element names must be unique` the second time it is pressed,
unless the author renamed the first one in between.

### Root cause

`addDefaultElement` named every new element `"element"` and
`addDefaultPage` named every page `"page"`. Resource names are unique **across**
elements and pages, not just within a kind
(`RecognitionCatalog::create` compares page names against element names).

### Fix

Take the first free `<stem>_N`, checked against element **and** page names.

### Regression check

Press New Element twice without renaming; both succeed as `element_1` and
`element_2`.

## An entity with no list panel is unreachable after creation

### Symptom

Only the just-created element can be edited. Creating a second one makes the
first permanently unreachable, and once the selected element stops being a
page anchor, `New Page` — which requires a selected page anchor — can never
succeed again.

### Root cause

`setSelectedElementId` had exactly one call site: immediately after
`New Element`. There was no element list. Pages were worse: visible only
inside the properties panel of whichever element happened to be selected, and
not deletable at all.

### Fix

A Elements panel and a Pages panel, alongside the existing Sources list.
Selecting an element also selects the source it was authored against —
otherwise its rectangles are drawn over whatever image the canvas happens to be
showing, which is a second latent defect the list would have exposed.

### Regression check

Manual: create two elements, select the first, confirm the properties panel
follows and the canvas switches to that element's source.
