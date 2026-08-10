# UmbraFlow

> **2026-08-09 breaking rewrite:** Runtime/annotation/action clauses in this
> file are historical where they conflict with the frozen
> [Runtime v2 authority](docs/plans/2026-08-09-runtime-hardening-rewrite.md).
> Page/Element/Hit/UFR, Context truth, direct task input, and compatibility APIs
> are not implementation targets.

A reusable game-automation host: C++ owns target/capture/input safety, trusted
Luau resolves a screenshot-free RuntimeModel, Operator exposes semantic tools,
and game projects connect only through a data-only ProjectPlugin.

## Canonical v1.7 vocabulary

- **ControlledTarget**: one attached window/process generation.
- **RuntimeArtifact**: a verified manifest, root `page-model.toml` and its
  manifest-listed assets; never an annotation screenshot bundle.
- **UiTarget**: semantic identity only.
- **Binding**: the sole owner of an actionable placement/variant.
- **StateResolution / BindingResolution**: separate same-cycle UI decisions.
- **Receipt**: Host-owned opaque one-shot delivery authority.
- **OperatorSession**: the only Script/Agent/Human Tool facade for snapshots,
  semantic tools and Operations.
- **ProjectPlugin / ProjectRegistration**: the only game extension boundary.

_Avoid as current contracts_: Context truth, Page, Element, Hit, UFR, direct
`ctx` action methods, direct run/check/replay input, caller coordinates,
caller effects and compatibility readers.

## Schema ids

Two ids travel in band — written into the payload as a `schema` field and
re-read on load:

- **`umbraflow-runtime/v2`**: the RuntimeModel, one `page-model.toml` per
  project. Pinned as `project.schema` and `model.schema` in
  `modules/task/runtime/{project,model}.luau`; field shapes in
  `schema/umbraflow-runtime-v2.schema.json`.
- **`umbraflow-trace/v2`**: the merged audit stream. Pinned as the `schema`
  `const` in `schema/umbraflow-trace-v2.schema.json` and as
  `trace::k_traceSchema` in `modules/trace/source/trace/event.hpp`.

`umbraflow-external/v1` names an external-payload manifest
(`scripts/fetch_external.py`, `modules/{ocr,operator}/external/manifest.toml`).
Every other file in `schema/` is identified by its `$id` alone and carries no
in-band id: annotation-workspace-v2, journal-v1, operator-v1, policy-v1,
project-registration-v1, runtime-artifact-v1.

A file name spells an id with a hyphen while the id itself carries a slash —
`umbraflow-trace-v2.schema.json` holds `umbraflow-trace/v2`. A directory
listing is therefore not evidence about which ids exist; read the `const` or
the constant that pins the file.

_Avoid as current contracts_: `umbraflow-authoring/v4`,
`umbraflow-annotations/v3`, `umbraflow-project/l2-v2`, every earlier spelling
of those three, and the constants that carried the first two,
`k_authoringDocumentSchema` and `k_runtimeManifestSchema`.

> Added 2026-08-11. The authoring and annotations lines went out with the C++
> model in `a80ea07` (2026-08-01); `umbraflow-project/l2-v2` was replaced by
> `umbraflow-runtime/v2` in `8af22bc` (2026-08-09). Design authority:
> [runtime hardening rewrite](docs/plans/2026-08-09-runtime-hardening-rewrite.md).
> `umbraflow-trace/v2` is untouched by either and remains live.

## Historical pre-v1.7 language

The material below is retained as terminology history only. It cannot define a
current API, schema or implementation when it differs from the authority above.

> Checked 2026-08-11, so the next reader need not re-derive it. These named
> things do not exist in the tree: `script-validator.hpp`, `scribe`, `FrontEnd`
> (and so `FrontEnd::Annotation`), `mint_receipt`, `resolve_page`, `walk_edge`,
> `stack_new`, `rect_override`, `cycle_match`, `isSubsetOf`,
> `ElementCapabilities`, `ExercisedCapabilities`, `BindingResolution`,
> `retry_attempt`, and the trace keys `steps`, `seq` and `elementId`.
> `resolve_page`, `find_element` and `mint_hit` are further refused outright by
> `RETIRED_RUNTIME_SYMBOL_PATTERN` in `tests/test-runtime-surface.py`. Four
> entries are wrong in a subtler way, the capability being live under another
> spelling: `frameworkProjectGlobals()` exists but returns an **empty**
> whitelist, so it publishes no `task` global and no other; the
> `umbra-flow check` verb is retired, the CLI dispatching only `explore` and
> `targets`; an exploratory click is written as `annotation.click_delivered` on
> the exploration stream rather than being an absence of
> `engine.action_delivered`; and crop is `view:crop` on the frozen view inside
> `explore.cycle`, not a top-level `explore.crop`.
> What does still hold: `engine::EngineSession`, `CaptureSessionId`,
> `TargetGeneration`, `FrameId`, `EngineRunId`, `trace::TraceStreamValidator`,
> `TraceRecorder`, `cycle-ledger.hpp`, `stream-validator.hpp`,
> `evidence.luau`, `observe.luau`, `project.load_project`, `explore.probe`,
> `error_tag`, `engine.action_delivered`, `searchRoi` as a C++ parameter name,
> and the retirement of the `drive` front-end in `eafc273`.

### Scripting

**Capability namespace (`uf`)**:
The single read-only global root through which a project task script reaches
the host: `uf.errors`, and nothing else. (Corrected 2026-08-08: this said
`uf.task` and `uf.errors`; `task` is its own project global published by
`frameworkProjectGlobals()`, never a member of `uf` -- the root carries data
alone, so there is no verb form under it to approve. `uf.pages` and
`uf.elements` were host-built
tables until 2026-08-01; pages and elements are trusted-Luau values now, reached
through `project.load_project`, and the tables are gone rather than empty. The
spellings `uf.elements.<name>` and `uf.pages.<name>` survive only as names a
pass before the VM resolves against the project file, `script-validator.hpp`.)
A script
sees nothing of the host outside this root, and effects reach it only through
the `ctx` object passed into a task's `run`. `uf` is the same product
abbreviation as the C++ `uf::` namespace, used deliberately in both languages.
_Avoid_: `uf.recognizers` (this table's spelling until 2026-07-31; the
capability model had already made it false, because the table holds exactly the
elements a script may CLICK and excludes the identify-only ones that do the
recognising), `umbra` (the 2026-07-27 spelling of this root; renamed to `uf` on
2026-07-29, see `docs/plans/2026-07-29-three-layer-task-system.md` §6 and §18 —
the rename touches only this script root, never the product names `UmbraFlow`
and `umbra-flow` or any schema id; the ids current when that was written were
`umbraflow-authoring/v4`, `umbraflow-annotations/v3` and the merged trace
schema `umbraflow-trace/v2`, which shipped on 2026-07-29 in `modules/trace`,
and only the last of the three still exists — see **Schema ids**
above), `bot` (superseded draft
wording in the grill decisions and the S0 annotation design)

> Corrected 2026-07-31: the two annotation schema ids read
> `umbraflow-authoring/v2` and `umbraflow-annotations/v1` here until the
> capability model landed. Both were bumped in one atomic change, and neither
> old id has a read path — an old schema string fails with the ordinary
> unsupported-schema error. Deciding artifact:
> `docs/plans/2026-07-31-annotation-model-capabilities.md` §三; in code,
> `k_authoringDocumentSchema` and `k_runtimeManifestSchema`.

> Corrected 2026-07-31 (vocabulary rename): all three ids moved again, because
> a key rename is a wire change. `umbraflow-authoring/v3` → `v4`
> (`[[annotation]]` → `[[element]]`, `[[variant]]` → `[[appearance]]`, a
> reference's `variant` → `appearance`, `recognizer_kind` → `element_kind`),
> `umbraflow-annotations/v2` → `v3` (`[[recognizer]]` → `[[element]]`, plus the
> same two appearance keys), and `umbraflow-trace/v1` → `v2` (`recognizerId` →
> `elementId`, the resources line's `recognizers` array → `elements`). No old id
> has a read path. In code: `k_authoringDocumentSchema`,
> `k_runtimeManifestSchema`, `k_traceSchema`.
>
> `[[annotation]]` is the third instance of the same defect and the oldest: it
> spelled the anchor/target/info taxonomy the capability model had already
> retired, so a persisted key named a classification that no longer exists. It
> moved inside the bump that was happening anyway rather than costing a v5.

> Corrected 2026-08-11 (both notes above): read them as a record of what was
> decided on 2026-07-31, not as a description of the tree. Two of the three
> lines they track are gone — `umbraflow-authoring/v4` and
> `umbraflow-annotations/v3` are in no file under `schema/`, `modules/`,
> `entry/`, `tools/` or `tests/`, and so are the constants they cite,
> `k_authoringDocumentSchema` and `k_runtimeManifestSchema`. They went out with
> the C++ model in `a80ea07` (2026-08-01); the RuntimeModel is
> `umbraflow-runtime/v2` now, whose vocabulary is Surface, Binding, `variant`,
> `ui_target`, `locator` and `reader`. `k_traceSchema` and `umbraflow-trace/v2`
> are the one part still true. Both notes name
> `docs/plans/2026-07-31-annotation-model-capabilities.md` as the deciding
> artifact; that plan was superseded on 2026-08-11 in `8be0b35`. The deciding
> artifacts are now
> [runtime hardening rewrite](docs/plans/2026-08-09-runtime-hardening-rewrite.md)
> for design, [the next block](docs/plans/2026-08-10-next-block.md) for
> requirement state, and
> [the W2-W7 reconciliation](docs/plans/2026-08-10-w2-w7-reconciliation.md)
> where the same-day specifications conflict.

**Private capability surface**:
The one host-built table of primitives that only the trusted Luau framework can
reach — the observation cycle, the bounded waits, `raise`, `emit`, `terminal`
and `random`. The primitives ARE keys of that table; what makes it private is
that the table itself has no name in either environment: the host hands it to
the framework bundle as its chunk argument, drops its own reference, and the
only way to reach it afterwards is the closure it was handed to. A project task
can neither name nor reach it; it sees only the `uf` capability namespace and
`ctx`. The table also carries one non-capability field, `error_tag`, the Tier B
label `ctx:try` compares against.
The roster and its four invariants are defined in
`docs/plans/2026-07-29-three-layer-task-system.md` §5 (2026-07-29).
_Avoid_: native driver, private native surface, raw verbs, "never a key of any
table" (the 2026-07-29 draft wording; §5's own tightening note replaced it,
because the primitives are exactly the keys of the private table — read
literally it says the code violates its own design)

**Framework environment**:
The writable globals table the trusted Luau framework's modules load under, and
the only environment whose metatable chains `__index` to the VM's main globals.
Environment isolation in Luau is **per closure, not per thread** — `luau_load`
takes the env a chunk closes over, and a new thread copies its parent's globals
— so this is a real table in the VM registry, never the `luaL_sandboxthread`
proxy shape, which is the `_G` escape shape the design exists to exclude.
_Avoid_: framework sandbox, trusted thread, framework `_G`

**Project environment**:
The globals table one project task run executes under: an explicit whitelist
with **no metatable at all**, so there is no `__index` chain to the framework
environment or the main globals. It is registered as a frozen prototype and
shallow-copied per run, so globals a run writes die with that copy. The absence
of the metatable is the isolation property itself; the denial list is a
consequence of it, not the mechanism.
_Avoid_: script sandbox, task environment, per-run `_G`

**Tier B error carrier**:
The value a recoverable automation failure is raised as: a userdata the host
mints under its own Luau tag, wearing a protected metatable labelled
`uf.error`. C++ decides what it is by the tag alone; the label is what tells it
apart from the other host userdata a script can hold (a page, a hit) and is
what `ctx:try` compares. A project script can mint no tagged userdata at all —
`setmetatable` and `table.clone` take tables, and `newproxy` is absent from
both environments — so the carrier cannot be forged however exactly a table
copies its fields.
_Avoid_: error table (the shape before 2026-07-29 `c37ee5b`, where identity
rested on a metatable a `table.clone` could carry along), exception, error
object

**Task**:
One automation flow (such as a game's daily routine) authored as a Luau
script and executed by the host in its own isolated VM against one target
window. A task always belongs to exactly one project and is addressed by its
name within that project, never by a loose file path.
_Avoid_: script (the source artifact a task is written in, not the unit of
execution), job, macro

**Project**:
Everything the host needs to automate one game target: the authoring
document, source screenshots, the generated runtime recognition assets, and
the tasks written against them.
_Avoid_: workspace, profile

### Runtime

**Engine session (`engine::EngineSession`)**:
The stateful engine capability scope that owns one loaded recognition runtime,
one frame source, one action sink, and one trace sink. It vends observations
and authorizes actions for one bound target. The name describes the lifetime
of that capability bundle; it does not imply that the object has a
`CaptureSessionId`.
_Avoid_: engine run (would conflate the capability scope with execution
identity and `EngineRunId`), capture session

**Capture session identity (`CaptureSessionId`)**:
The outer component of frame identity. Together with `TargetGeneration` and
`FrameId`, it prevents evidence from distinct capture sessions from
colliding. It is supplied by the composition root and belongs to captured
frames, not to `engine::EngineSession`.
_Avoid_: `SessionId` (too generic), engine session id, task session id

**Observation cycle**:
The explicit open/close scope around exactly one capture, inside which page
resolution, element lookup, and a single click all read the same frame.
Opening it costs one capture; closing it releases the frame deterministically
and is idempotent. It replaces GC-driven frame lifetime, and because a page and
a hit from one cycle are same-frame by construction, cross-frame evidence
mismatch stops being checkable state and becomes structurally impossible.
Defined in `docs/plans/2026-07-29-three-layer-task-system.md` §4 (2026-07-29).
_Avoid_: frame box, observation lease (the freshness contract on a delivered
action, not this scope), capture scope

**Ticket**:
The script-side handle to one open observation cycle. C++ owns the ticket
ledger and re-checks the ticket on every use; when the cycle closes, the ticket
is dead and every later operation on it fails. The ticket is the unit the host
invalidates after an action, and it carries no data the script can read.
It names its cycle by two numbers, both re-checked: the ledger's process-wide
generation stamp and the cycle's ordinal within that ledger. The stamp is what
makes a ticket left over from a spent generation get rejected rather than
collide with a live ordinal in the next one.
Defined in `docs/plans/2026-07-29-three-layer-task-system.md` §4 (2026-07-29);
implemented in `modules/task/source/task/cycle-ledger.hpp`.
_Avoid_: frame handle, cycle object, token (reserved for `std::stop_token`)

**Receipt**:
What `observe.resolve_page` hands back when a page resolved, and the thing
`observe.click`, `observe.long_press` and `observe.walk_edge` refuse to act
without. It is not a boolean dressed up: it names the page that resolved AND
the ticket it resolved on, and both halves are re-checked at the acting verb,
so a receipt earned on one frame cannot authorise a click on the next. A hit on
a page-positioned element is worth exactly this receipt and nothing without
one. The framework mints receipts through `evidence.mint_receipt` and keeps the
ledger of every one it minted, which is why `evidence` is the one framework
module NOT published to project scripts: a script that could name it could mint
a receipt for a page nothing resolved.
Defined in `docs/plans/2026-07-31-script-owned-page-model.md`; implemented in
`modules/task/runtime/evidence.luau` and enforced in
`modules/task/runtime/observe.luau`.
_Avoid_: page token (token is reserved for `std::stop_token`), page proof,
resolve result (all three lose the fact that it names a ticket as well as a
page, which is the half that makes it same-frame)

**Trace stream validator (`trace::TraceStreamValidator`)**:
The state machine one run's evidence stream must pass. It exists because the
`framework.*` events are the only ones the trusted Luau framework *requests*
rather than the host authoring, so `emit` is not a passthrough: a framework
whose step nesting, retry counting, or interrupt matching had drifted would
otherwise write a plausible audit log of a run that did not happen that way.
`TraceRecorder` owns one and runs it on every event before the stamp, and the
recorder is the only path to a sink in the codebase, so it cannot be gone
around. Its refusals carry two kinds, and the split is the point: a request it
declines because a project caused it (an over-long or ill-encoded step name, a
depth or payload ceiling) is Tier B `InvalidResource` and the generation lives,
while a protocol breach only this binary can produce is `InternalInvariant` and
spends it.
Defined in `docs/plans/2026-07-29-three-layer-task-system.md` §12 (2026-07-29);
implemented in `modules/trace/source/trace/stream-validator.hpp`.
_Avoid_: trace schema validator (the schema is the wire format; this validates
the sequence), emit guard, event sanitizer (nothing is sanitized — a refused
event is rejected at the request boundary, never truncated)

**Open step path**:
The framework step scope that was open when a trace line was written, stamped
onto that line by the validator rather than supplied by the emitter — part of
the stamp for the same reason `seq` is, because an emitter does not get to say
which steps it is inside. It appears on the wire as a `steps` array, present
only when non-empty. It is **not** a unique address within a run: a `for` loop,
a `ctx:retry` body, and an interrupt firing up to `max_hits` all legitimately
reopen a sibling name under one parent, so the same path recurs and
`retry_attempt` is what distinguishes a repeat.
_Avoid_: step id, step address, step key (all three imply the uniqueness the
2026-07-29 draft asked for and `4030ffd` dropped as unenforceable)

### Annotation model

Added 2026-07-31 with the capability model; **relocated 2026-08-01**. Semantics:
`docs/plans/2026-07-31-annotation-model-capabilities.md`. Where they live:
`docs/plans/2026-07-31-script-owned-page-model.md`. The nouns below and what
they mean did not change; their spellings did.

Element, page, reference, appearance and edge are **trusted-Luau types** now —
`modules/task/runtime/{model,navigation,oracle}.luau` — persisted to
`umbraflow-project/l2-v2` (`page-model.toml`, written by `project.luau`). C++
keeps pixels, tickets and guarantees, and the `annotation::` spellings that
appear below are history, not alternatives.

> Corrected 2026-08-11: the paragraph above is history, and every entry under
> this heading with it. Three of its specifics are false of the tree. The file
> is written by `project.luau` under `umbraflow-runtime/v2`, not
> `umbraflow-project/l2-v2` — that id was replaced in `8af22bc` (2026-08-09).
> `navigation.luau` and `oracle.luau` do not exist; `modules/task/runtime/`
> holds `evidence`, `explore`, `jcs`, `model`, `observe`, `project` and
> `resolution`. And the nouns did not survive the rewrite: `Capabilities`,
> `Holding`, `exercised`, `Appearance` and `ElementId` are types nowhere in the
> tree, the RuntimeModel vocabulary being Surface, Binding, `variant`,
> `ui_target`, `locator` and `reader` instead. Read the entries below for what
> the words once meant, never for what exists. Deciding artifacts: `a80ea07`
> (2026-08-01) and
> [runtime hardening rewrite](docs/plans/2026-08-09-runtime-hardening-rewrite.md).

**Element (`model.Element`)**:
What an author draws: the set of uses one patch of the target's screen may be
put to, the appearances it can take, and — where the element has one — the
rectangle to look in. It is project-level — nothing on it says which page it
belongs to. This is the noun the authoring CLI, the authoring document, the
editing layer, the script surface (`uf.elements`), and the trace (`elementId`)
all speak in.
_Avoid_: recognizer (the pre-2026-07-31 spelling; an element that only reads
text or only receives a click recognizes nothing, and is located by the page it
sits on), region, annotation (the old three-way kind), `RecognizerId` (renamed
to `ElementId`)

> Corrected 2026-08-02: `rect` used to be required, and this entry used to open
> with "one rectangle of the target's screen". It has only ever meant WHERE TO
> LOOK — `observe.find` searches it and reports the hit's own position — so some
> elements have no answer of their own: a minimap cell is matched at coordinates
> the script works out per frame because the map pans, and one confirm button
> drawn once sits somewhere different on every screen that shows it. Both used to
> carry the rectangle they were cut from, which the model then stated as a fact
> nothing could contradict.
>
> **A rectangle is supplied by the element, by the page reference
> (`rect_override`), or by the falsification claim (`oracle.Expectation.rect`),
> and for any one use exactly one of the three supplies it.** Absent on the
> element therefore means "the caller says where", never "nobody said": `Page.new`
> refuses a row that neither inherits one nor states one, `Expectation.new`
> refuses a cell that does the same, and an element with no rectangle can never be
> part of a page signature, because the identify sweep runs before the page is
> known and so has only the element's own to search. In code:
> `modules/task/runtime/{model,oracle,observe,project,regress}.luau`.

**Compiled element (retired 2026-08-01)**:
The compiler that emitted one runtime artifact per element is gone with the v4
authoring line: the layer-two model IS the runtime form, and a template is a
PNG blob `template_load` turns into a handle. Kept as an entry because the word
appears throughout the plans written before the migration.
_Avoid_: `CompiledElement`, `CompiledAppearance`, `RuntimeElementSpec`,
`RecognizerDefinition` (all deleted), reading any of them as a live type

**Capability set (`element.capabilities` / `reference.exercised`)**:
The three uses one patch of pixels can be put to — `identify`, `interact`,
`read` — held as a set rather than a choice, so an element that both names its
page and can be clicked is one element matched once per cycle. Each capability
carries its own payload, which is what makes "an element without `read` has
nowhere to put OCR parameters" a fact of the type. Two types, not one: the
element declares what it CAN do (`ElementCapabilities`), a page's reference
declares what THAT page does with it (`ExercisedCapabilities`), and the second
must be `isSubsetOf` the first. Empty is rejected on both sides.
_Avoid_: `AnnotationType`, `ElementKind`, `AnchorElement`, `InteractiveElement`,
`InfoElement`, `PageAnchor`/`ActionTarget`/`InfoRegion` as element kinds (the
2026-07-26 three-way enum, replaced 2026-07-31), bitmask

**Page reference (`model.Reference`)**:
One page's use of one element: `{pageId, elementId, holding, exercised,
rect_override?, appearance?}`. It is the edge the model is built on — authorisation IS
the reference, and a page's signature is *derived* from the references whose
`exercised` identify carries `Required` or `Forbidden`, never authored. `holding`
is `Owned | Referenced`: an authoring-side editing guard rail the runtime never
reads, recording which page an element is at home on — exactly one `Owned` row
per element, every other page that uses it `Referenced`. Ownership is not
exclusivity: every drawn element is `Owned` by the page it was drawn on, so
reading `Owned` as "refuse to reference these pixels elsewhere" would make
`page reference` fail for every element there is.
_Avoid_: `searchRoi` (the pre-2026-08-02 name for `rect_override`, and still the
spelling of an unrelated C++ parameter in engine/vision/task),
`allowed_page_ids` / `allowedPageIds` (the separate authorisation list
the reference replaced), `bool shared` (an intent flag that could contradict the
placements with nothing noticing), `PageSignature::create` (a signature has no
public factory now, because a second way to state one fact could disagree with
the first), placement (the pre-2026-07-31 name for the same row, when it carried
neither holding nor exercised capabilities)

**Appearance (`model.Appearance`)**:
One named appearance of one element. An appearance changes what a patch of
pixels LOOKS like; it does not change where that patch is or what it means.
Named rather than indexed because for a 1x/2x/3x speed button the matched form
IS the state and the name has to reach the script surface. Declaration order
decides ties and nothing else. An empty appearance list is legal and means the
rectangle is located by the page being recognised rather than by pixels of its
own — such an element can never be identity evidence.
_Avoid_: variant (the spelling until 2026-07-31; the CLI verb was already
`element appearance`, and the word also collides with `std::variant` — which
keeps its own name, as does the UUID variant field), form, appearance-kind,
template list (an appearance is more than its template rectangle)

**Page graph and stack (`navigation.Edge`, `navigation.stack_new`)**:
Where an edge leads is a falsifiable fact about the game and lives in the
project file; whether to take it is policy and lives in a task. An edge names
its from-page, a SET of destinations, a trigger (`click` a reference / `key` a
name / `spontaneous`) and a kind (`navigate` / `push` / `pop`). Overlays push
and pop against a runtime stack with a declared depth cap, and a `pop` names no
destination because the target is whatever the stack shows beneath it. **The
stack is belief and an observation is truth**: resolving any non-overlay base
page resets it to that page.
_Avoid_: pathfinding in the host (a `go(any page)` verb — the framework offers
`walk_edge` only), per-page depth or z-order declarations, modelling an overlay
as an ordinary edge

**Falsification matrix (`oracle`, `reading`, `recognition`, `regress`,
`umbra-flow check`)**:
The screens a model is measured against, what each cell is supposed to show
(`match` / `absent` / `unclaimed`), and the walk that measures them. Scores are
layer one (`cycle_match` over file-backed frames); judging is layer two, and four
rules fire without any `[[expect]]` at all: two appearances of one element may
not both hit one screen, a winner must beat every rival by the separation factor,
one region may not be claimed to read one text on two screens that are two pages,
and a screen that says which PAGE it is of must have that page resolve on it.
A screen's `page` is optional (a capture of a page nobody has annotated yet is
still measurable) and is what separates two views of one page — a scrolling grid
photographed twice — from two pages resting on one word.
A cell is `(screen, element, appearance?, rect?)`, and the rectangle is in it
because an element that draws none of its own is measured wherever its claims
place it: one screen can hold several rows naming one element, which is how "this
shape matches HERE and stays away from THERE" becomes two measurements instead of
an argument. "One region reads one text on two screens" is therefore keyed by the
region — nine confirm buttons are one element and nine rectangles, and reporting
those as nine models that cannot tell nine screens apart is a rule an author
switches off.
_Avoid_: recording measurements back into the file as expectations (a run that
writes down what it measured agrees with itself by construction), reading an
undeclared `page` as "same page as the other one" (silence is not a fact),
checking that a screen resolves NO other page (an overlay legitimately resolves
its base page), `umbra-authoring check` (the v4 verb this replaced)

**Exploration environment (`explore`, `scribe`)**:
The second trusted-Luau environment, the Agent's operating table: every verb a
task has, plus the privileged ones — a bare-coordinate click, `explore.crop`
(pixels out) and `explore.probe` (colour statistics). A business environment can
name none of them. Its runs stamp `FrontEnd::Annotation`, whose trace stream
structurally refuses `engine.action_delivered`, because a bare click has no
element and no page and the vocabulary has to stay honest.
_Avoid_: handing a task raw pixels or a bare click, `engine.action_delivered`
for an exploratory click, "operator mode" (the `drive` front-end was a separate
consumer with no model access at all; it was retired into this one on
2026-08-03 in `eafc273`, so the phrase now names nothing)
