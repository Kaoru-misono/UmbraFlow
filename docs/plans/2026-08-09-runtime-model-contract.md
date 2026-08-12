# Umbraflow Runtime Model Contract v1

Status: frozen for P0 implementation

Date: 2026-08-09

> **Amended 2026-08-12 for `a1140a4`, which added readings to the runtime
> model.** The amended passages are §2.6, §2.7, §3 and §4, each marked where it
> starts. Two facts about the document as a whole belong with them, because the
> amendments are written in names the rest of it does not use. The field-shape
> authority is
> [`umbraflow-runtime-v2.schema.json`](../../schema/umbraflow-runtime-v2.schema.json),
> whose exact bytes the runtime-model file reader in `modules/task/` pins as
> `k_runtimeModelSchemaHash`; the four v1 schema files listed below were deleted
> with the surface they described, and `tests/test-runtime-surface.py` keeps
> them deleted. And the
> record vocabulary of §2 and §4 is the pre-v2 spelling — `Context`, `Target`,
> `Identity`, `schema_version = 1` — while a model on disk is
> `schema_version = 2` over `ui_target`, `locator`, `reader`, `binding`,
> `surface` and `transition`. The amendments use the schema's names. Reconciling
> the rest of §2 and §4 to them is a separate change and has not been made.

This document is the field-level contract for the annotation-system rewrite.
It deliberately does not preserve the old `Page`, `Element`, `Reference`,
`CapabilitySet`, `holding`, `exercised`, `screen`, or `expect` runtime model.

The machine-readable contracts are:

- [`umbraflow-runtime-v1.schema.json`](../../schema/umbraflow-runtime-v1.schema.json): runtime model and runtime result shapes;
- [`umbraflow-offline-v1.schema.json`](../../schema/umbraflow-offline-v1.schema.json): frames, observations, assertions, candidates, conflicts, and patches;
- [`umbraflow-annotator-api-v1.schema.json`](../../schema/umbraflow-annotator-api-v1.schema.json): backend/UI messages;
- [`umbraflow-cpp-envelope-v1.schema.json`](../../schema/umbraflow-cpp-envelope-v1.schema.json): pre-VM facts.

The JSON schemas define field shape. The cross-object rules and runtime
semantics below are part of the contract and must be implemented by the one
semantic validator.

## 1. Boundary and authority

```text
offline frames / OCR / Agent proposals / review
                         |
                         | validate + accept + compile
                         v
runtime-model.toml + content-addressed locator assets
                         |
                         v
Umbraflow runtime
```

The runtime package contains no annotation screenshots, raw OCR observations,
Agent reasoning, assertions, review records, or regression matrices. A runtime
observation cycle may hold a temporary live frame, but that frame is not the
offline corpus.

`umbraflow-runtime-v1.schema.json` is the single field-shape authority for
the `RuntimeModel` encoded by `runtime-model.toml`, runtime model values,
resolution values, and receipts. The
Luau compiler is the single semantic validator. Python and TypeScript consume
the schema and compiler protocol; they do not maintain independent enums.

## 2. Runtime concepts

### 2.1 Context

```text
Context { id }
```

`Context` is a business/runtime environment, such as `camp` or
`chaos_battle`. It is not a screenshot, a locator, or a visual page. Context
IDs are unique in one model.

### 2.2 Surface

```text
Surface {
    id,
    kind = scene | overlay | interrupt,
    contexts: ContextId[],
    covers?: SurfaceId[]
}
```

`scene` is the base visible layer. `overlay` covers compatible lower surfaces.
`interrupt` can appear independently of the current scene. There is no
authored catch-all surface. Unknown UI is a runtime result and cannot grant an
action.

Rules:

- `scene.contexts` is non-empty and `covers` is absent;
- `overlay.contexts` and `covers` are non-empty; `covers` names only lower
  `scene` or `overlay` surfaces and never an interrupt;
- `interrupt.contexts` may be empty, meaning any context, and `covers` is
  absent;
- all referenced contexts and surfaces must exist;
- a surface must have at least one binding with a positive identity predicate;
- a surface identity may not consist only of forbidden (`none`) predicates.

### 2.3 RuntimeState

```text
RuntimeState {
    context: ContextId,
    surfaces: SurfaceId[]  // bottom to top, exact visible order
}
```

`surfaces` is non-empty and contains no duplicate ID. The bottom item is a
scene. An overlay is valid only above a surface named by its `covers` relation;
an interrupt is the top layer when present.

### 2.4 Target

```text
Target {
    id,
    kind = control | label | indicator | collection | region,
    geometry?: Geometry,
    locators: LocatorId[],
    readers: ReaderId[]
}
```

A Target is a reusable semantic object. It does not identify a Surface and it
does not grant an action. Shared targets are represented by multiple bindings;
there is no runtime ownership field.

`Geometry` is one of:

- `fixed { kind, rect }`;
- `collection { kind, rect, axis, item_size, spacing, max_items? }`;
- `relative { kind, anchor: LocatorRef, offset }`.

### 2.5 Locator and LocatorVariant

```text
LocatorRef { locator: LocatorId, variant?: LocatorVariantId }
```

`Locator` is one of:

- `template`: a content-addressed template asset, optional search rectangle,
  threshold, and/or variants;
- `text`: an OCR reader and optional search rectangle;
- `geometry`: a fixed rectangle;
- `relative`: an anchor locator and offset;
- `collection`: an item locator, axis, spacing, and optional maximum count.

`LocatorVariant` is a concrete template asset variant belonging to one
template locator. A variant inherits the parent locator's threshold and search
rectangle when it does not override them. Template locators must have either
an inline `asset` or at least one variant.

Asset paths are relative to the runtime asset root, use `/` separators, and
cannot escape the root. Every asset carries a lowercase SHA-256 digest.

### 2.6 Reader

`Reader` is a target-specific decoding contract:

```text
line      { id, target, kind, confidence_floor, normalization }
block     { id, target, kind, confidence_floor, normalization, join }
items     { id, target, kind, confidence_floor, item_kind, min_items?, max_items? }
presence  { id, target, kind, confidence_floor }
```

`normalization` is `raw`, `trim`, or `collapse_whitespace`. Block `join` is
`newline` or `space`. A measured confidence below `confidence_floor` produces
`Unknown`, never `Absent`.

> **Amended 2026-08-12 (`a1140a4`).** A Reader's value now leaves the resolver.
> A Binding names Readers in `reads` (§2.7), and a resolved state reports the
> normalised value of every such Reader it measured, attributed to the UiTarget
> that Binding names. `confidence_floor` therefore decides reporting as well as
> evidence: a measurement below the floor is absent from the reported readings
> rather than reported with a reason and a score. The floor is the trusted
> Reader's judgement, and a caller handed the failure could re-decide it. A
> Reader costs one Host read out of the observation cycle's pool, so a model
> that declares more `reads` than one cycle can pay for reports fewer readings
> and never a wrong one.

> **Amended 2026-08-13: a Reader declares its text layout.** Every Reader
> carries a required `layout`, either `single_line` or `block`, with no default
> and nothing inferred. `single_line` asserts the rectangle holds exactly one
> line and skips line detection; `block` says the count is not known, so every
> line the detector finds inside that rectangle is located and read. A block
> Reader costs one Host read for the detection pass plus one per line located,
> out of the same cycle pool. `text_equals` over a Reader holds only when the
> reading is exactly one line equal to the value, which is a single_line
> Reader's behaviour unchanged and refuses to invent a separator for a block.

### 2.7 Binding

```text
Binding {
    id,
    surface: SurfaceId,
    target: TargetId,
    placement: Placement,
    identity: Identity,
    readers: ReaderId[],
    actions: Action[]
}
```

`Placement` is `target_geometry`, an explicit `rect`, or a relative anchor
and offset. `readers` declares which target readers are exposed on this
surface. `actions` declares the only actions that this surface grants for the
target.

`Identity` has three predicate lists:

```text
Identity {
    all: IdentityPredicate[],
    any: IdentityPredicate[],
    none: IdentityPredicate[]
}
```

`all` predicates must be present, at least one `any` predicate must be present
when the list is non-empty, and every `none` predicate must be absent. An
unknown result blocks resolution. A binding with no identity predicates is
allowed for a read-only or action-only target, but cannot be the only identity
evidence for its Surface.

An identity predicate is either:

```text
{ kind = "locator_present", locator: LocatorRef }
{ kind = "text_match", reader, operator = "equals", value: string }
{ kind = "text_match", reader, operator = "contains_all", value: string[] }
{ kind = "text_match", reader, operator = "present" }
```

The reader in a text predicate must belong to the binding target.

> **Amended 2026-08-12 (`a1140a4`).** A Binding also carries `reads`: the IDs of
> the Readers whose value it reports once its own predicates measure it present.
> `reads` is optional and absent is the empty list — requiring it would put a
> field on every Binding to record that it reports nothing. Every ID must name a
> declared Reader, and no ID may repeat. A Reader named in `reads` reports and
> never decides: it takes no part in this Binding's identity, which remains the
> three predicate lists above.

### 2.8 Action

An action is a typed, binding-local authorization contract. It has a unique ID
within its binding and one of these shapes:

```text
{ id, kind = "click", locator: LocatorRef, preconditions: Predicate[] }
{ id, kind = "key", key, locator?: LocatorRef, preconditions: Predicate[] }
{ id, kind = "scroll", axis, delta, locator?: LocatorRef, preconditions: Predicate[] }
{ id, kind = "drag", locator, destination, preconditions: Predicate[] }
```

> **Amended 2026-08-12: two of these four exist.** The shipped shape is
> `{ id, kind, proof_locator }` plus `key` when `kind` is `"key"`, and `kind` is
> `"click"` or `"key"` and nothing else — `$defs.binding_action` in
> `umbraflow-runtime-v2.schema.json`, built by `action()` in
> `modules/task/runtime/model.luau`. `scroll` and `drag` were never implemented
> and are proposals here rather than a surface anything refuses.
>
> Two consequences the v1 text does not carry. Which key names exist is not
> decided by this contract: `uf::KeyName` is the single definition, it is applied
> to every key a model declares before the artifact is bound, and no second copy
> of the set exists. And `placement.action_point` is present exactly when some
> action is a `click`, so a Binding granting only keystrokes carries no
> coordinate at all.

`destination` is a point or locator reference. Preconditions are positive
locator/text predicates and must all be present in the same observation cycle.
An unknown precondition denies authorization. An action cannot refer to a
locator, reader, or target outside its binding target.

### 2.9 Transition

```text
Transition {
    id,
    from: StatePattern,
    trigger: action | timeout | external,
    to: StatePattern,
    verification: { kind = "resolve", timeout_ms, attempts }
}
```

`StatePattern` contains an exact context and an exact bottom-to-top surface
stack. The runtime model stores declared transitions only. Observed
transitions and expected test transitions are offline records.

## 3. Runtime resolution and receipt

Every resolution uses one observation cycle:

```text
open cycle
  -> capture one frame
  -> evaluate all applicable surfaces
  -> evaluate all identity predicates
  -> build a surface stack
  -> return Resolution
close cycle
```

Evidence is tri-state:

```text
Present(value, confidence, proof)
Absent(proof)
Unknown(reason, confidence?, proof?)
```

`Unknown` includes not measured, low confidence, OCR unreadable, locator
failure, incompatible geometry, stale frame, and internal error. Lack of a
measurement is never negative evidence.

`Resolution` is exactly one of:

```text
Resolved { kind, state, evidence[] }
Ambiguous { kind, candidates[2..], conflicts[1..], evidence[] }
UnknownResolution { kind, reason, evidence[], diagnostic? }
```

There is no first-match behavior. Candidate ordering is only a deterministic
search order: interrupt, compatible overlays, scene. Within a layer, a tie or
insufficient evidence margin produces `Ambiguous`. Context and `covers` rules
must be satisfied before a candidate can enter the stack.

> **Amended 2026-08-12 (`a1140a4`): readings.** A `Resolved` result also reports
> what it read, through a second verb over the same open cycle. One reading is
>
> ```text
> Reading { ui_target, reader, kind, lines?, reason? }
> ReadingLine { rect, text }
> ```
>
> with `kind` one of `read`, `absent` or `unknown`, `lines` present exactly when
> it read, and `reason` present exactly when it could not. A reading is a list
> of lines under either layout: a single_line Reader reports one element rather
> than a second shape. The list is sorted by
> `ui_target` then `reader`, so one world produces one document, and it carries
> one entry per Reader every reporting Binding named — the failures included,
> because a plugin that cannot separate "nothing is written here" from "this
> frame was unreadable" has to fail closed on one blurry capture. The score is
> still not carried: it has already been compared against the Reader's own
> `confidence_floor`, and it differs between two captures of one unchanged
> screen while this document is hashed into a decision basis. The schema calls
> the list `state_readings` and one entry `binding_reading`.
>
> A reading is attributed to the UiTarget and not to the Binding that carried
> it, because a Binding is a UiTarget plus a visual variant: the variant a hover
> or a highlight selects differs between two captures of one semantic instance,
> and the serialized resolution is hashed into `state_resolution_hash` and
> through it into `decision_basis_hash`, so a binding ID would move the decision
> on a hover. The confidence score is excluded for the same
> reason and not for economy — a score differs between two captures of one
> unchanged screen, and a recapture that is semantically equal must remain one
> decision. The pre-normalisation text is excluded with it; the raw read is what
> the Reader exists to narrow.
>
> **Amended 2026-08-13: each line's rectangle is carried, and IS hashed.** Under
> `block` that rectangle is the detector's measurement, in the coordinate space
> of the image, and two captures of one unchanged screen can move it — measured
> on a real frame, the same three lines came back one to two pixels apart when
> the surrounding region changed. It is hashed with the rest of the document
> anyway, which is the ruling: the score has no decision left in it once the
> floor has judged it, while the rect is where a plugin acts, and a decision
> basis that omits an input a plugin can branch on would let a frozen plan be
> re-derived into a different action without the basis moving. Unsoundness
> outranks an extra approval. Under `single_line` the rect is the Binding's
> declared placement and cannot move at all.
>
> A Binding contributes a reading only while its own predicates measure it
> present, because a Reader reads that Binding's rectangle and reading through a
> Binding that is not showing returns whatever is at those pixels instead. Two
> present Bindings that share a UiTarget report nothing for it: which instance
> is on screen is exactly what an ambiguity means, and resolution already
> refuses to guess it. A reading that did not clear its Reader's
> `confidence_floor`, or that was unreadable, is absent from the list rather
> than present with a reason.
>
> A reading is not evidence. It carries no evidence ID, it never appears in a
> receipt's `evidence_ids`, and it authorizes nothing. `Ambiguous` and
> `UnknownResolution` report no readings, because a state that resolved no
> Surface has nothing to attribute one to.

Only a framework-minted `Resolved` result can mint a `Receipt`. A receipt is:

```text
Receipt {
    kind = "receipt",
    id,
    ticket_id,
    cycle_id,
    frame_id,
    model_hash,
    state: RuntimeState,
    surface: SurfaceId,
    binding: BindingId,
    action: ActionId,
    evidence_ids: EvidenceId[],
    authorization: { ticket_id, cycle_id, target, action }
}
```

`observe.click` and all other action verbs must verify that the receipt is
framework-minted, belongs to the current ticket and cycle, uses the current
model hash, names the requested binding action, and has fresh proof from the
same cycle. `Ambiguous` and `UnknownResolution` never mint a receipt.

The JSON shape is not a construction authority. User Lua tables with the same
fields are not receipts; minting remains private to the trusted framework.

## 4. `runtime-model.toml` field contract

The TOML file maps directly to `RuntimeModel`:

```text
schema_version       integer, required, exactly 1
base_resolution      [positive integer, positive integer], required
base_dpi             [positive integer, positive integer], required
[[context]]           Context
[[target]]            Target
[[locator]]           Locator
[[locator_variant]]   LocatorVariant
[[reader]]            Reader
[[surface]]           Surface
[[binding]]           Binding
[[transition]]        Transition
```

All record IDs are unique within their record type. Unknown fields are a
validation error. Empty arrays are explicit: `locator_variant`, `reader`, and
`transition` may be empty; the other runtime record collections have the
minimums in the machine schema.

> **Amended 2026-08-12 (`a1140a4`).** A binding record may carry
> `reads = ["reader_id", ...]`. It is the one authored field readings add: it is
> optional, absent is the empty list, every ID must name a declared reader
> record, and a repeated ID is a validation error. A reading itself is a runtime
> value and is never written to `runtime-model.toml`. The fragment below predates
> the v2 model and the current schema rejects it; write `reads` against the
> schema named at the head of this document, not against the fragment.

The following is a complete valid fragment:

```toml
schema_version = 1
base_resolution = [1920, 1080]
base_dpi = [96, 96]

[[context]]
id = "camp"

[[target]]
id = "confirm_button"
kind = "control"
locators = ["confirm_button_visual"]
readers = ["confirm_button_text"]

[target.geometry]
kind = "fixed"
rect = [100, 200, 180, 64]

[[locator]]
id = "confirm_button_visual"
target = "confirm_button"
kind = "template"
threshold = 0.92
variants = ["confirm_button_enabled"]

[[locator_variant]]
id = "confirm_button_enabled"
locator = "confirm_button_visual"
asset = { path = "templates/confirm_enabled.png", sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" }

[[reader]]
id = "confirm_button_text"
target = "confirm_button"
kind = "line"
confidence_floor = 0.80
normalization = "trim"

[[surface]]
id = "camp_scene"
kind = "scene"
contexts = ["camp"]

[[surface]]
id = "training_confirm"
kind = "overlay"
contexts = ["camp"]
covers = ["camp_scene"]

[[binding]]
id = "training_confirm_confirm_button"
surface = "training_confirm"
target = "confirm_button"
placement = { kind = "target_geometry" }
readers = ["confirm_button_text"]

[binding.identity]
all = [
  { kind = "locator_present", locator = { locator = "confirm_button_visual", variant = "confirm_button_enabled" } },
  { kind = "text_match", reader = "confirm_button_text", operator = "equals", value = "确认" }
]
any = []
none = []

[[binding.actions]]
id = "click"
kind = "click"
locator = { locator = "confirm_button_visual", variant = "confirm_button_enabled" }
preconditions = []

[[transition]]
id = "camp_to_training_confirm"
from = { context = "camp", surfaces = ["camp_scene"] }
trigger = { kind = "external", name = "training_opened" }
to = { context = "camp", surfaces = ["camp_scene", "training_confirm"] }
verification = { kind = "resolve", timeout_ms = 2000, attempts = 3 }
```

The semantic validator additionally checks every reference, target/locator
ownership, reader ownership, locator variant parent, Surface stack legality,
unique action IDs within a binding, action references, and transition state
references. JSON Schema alone cannot enforce these relationships.

## 5. Offline evidence and CandidateModel

Offline objects use `umbraflow-offline-v1.schema.json` and never appear in the
runtime package.

### Frame

```text
Frame {
    id,
    asset: { path, sha256, width, height },
    captured_at,
    source: exploration_trace | manual_capture | import,
    tags[]?
}
```

The asset path points to the annotation corpus. Its hash identifies the exact
frame used by observations; it is not a runtime locator asset.

### Observation

```text
Observation {
    id,
    frame_id,
    subject: target | surface | region,
    measurement: template_score | ocr | geometry | item_count,
    classification: present | absent | unknown,
    confidence,
    provenance
}
```

An observation is an Agent or tool measurement, not a model assertion. OCR
measurements retain text, line boxes, confidence, and the measured rectangle.

### Assertion

```text
Assertion {
    id,
    frame_id,
    claim: surface_identity | target_locator | text |
           action_permission | surface_stack,
    outcome: present | absent | unknown,
    supporting_observations[],
    review: open | accepted | rejected | superseded,
    provenance
}
```

No assertion means unreviewed. It does not mean absent. An assertion marked
`unknown` is a reviewed uncertainty and cannot be compiled as negative
runtime evidence.

### CandidateModel

```text
CandidateModel {
    id,
    project_id,
    revision,
    status: candidate | accepted | rejected | compiled,
    source_frame_ids[],
    targets: CandidateTarget[],
    entities: CandidateGeneric[],
    patches: Patch[],
    conflicts: Conflict[]
}
```

`CandidateTarget.value` is a runtime Target. Other candidate entities use the
same runtime object definitions in their `value` field and declare
`entity_kind`. A CandidateModel may be incomplete, but it must never be
silently treated as an accepted runtime model.

### Patch

A Patch is semantic, not a raw TOML edit:

```text
Patch {
    id,
    change: create_entity | set_field | grant_action | revoke_action |
            merge_entities | split_entity,
    summary,
    confidence,
    risk: low | medium | high | critical,
    evidence_ids[],
    conflict_ids[]?,
    provenance,
    status: proposed | accepted | rejected | applied | superseded
}
```

High and critical action patches require explicit human acceptance. Agent code
may propose and validate them but may not apply them to accepted runtime data.
Unknown-to-known promotion, arbitrary ambiguity resolution, and automatic
shared-target ownership are prohibited patch changes.

## 6. Annotator backend/UI API

The backend exposes JSON over the existing local tool transport. The API is
revisioned and uses optimistic concurrency through `candidate_revision`.
Every mutating request must include the revision it reviewed; a stale revision
returns `revision_conflict` and changes nothing.

| Method and path | Request | Response |
|---|---|---|
| `GET /api/schema` | none | `SchemaManifest` |
| `GET /api/candidates` | `status`, `cursor` query | `ListCandidatesResponse` |
| `GET /api/candidates/{id}` | none | `CandidateModel` |
| `POST /api/candidates/{id}/patches/{patch}/accept` | `DecisionRequest` | `DecisionResponse` |
| `POST /api/candidates/{id}/patches/{patch}/reject` | `DecisionRequest` | `DecisionResponse` |
| `POST /api/candidates/{id}/compile` | `CompileRequest` | `CompileResponse` |
| `POST /api/candidates/{id}/validate` | revision | `ValidationResponse` |
| `GET /api/candidates/{id}/conflicts` | none | conflict list from CandidateModel |
| `GET /api/candidates/{id}/provenance` | none | provenance records |

The backend operations are exactly:

```text
list_candidates()
get_candidate(id)
accept_patch(candidate_id, patch_id, candidate_revision)
reject_patch(candidate_id, patch_id, candidate_revision)
compile_candidate(candidate_id, candidate_revision, write=false)
run_validation(candidate_id, revision)
get_conflicts(candidate_id)
get_provenance(candidate_id)
```

The UI is a decision queue, not a raw TOML editor. It must show supporting
frames, competing Surface candidates, semantic patch, expected runtime effect,
blast radius, conflicts, and validation state. It must not expose the deleted
Page/Element/Reference vocabulary. `write=false` is the default compile mode;
the backend writes `runtime-model.toml` only after the candidate is accepted and
validation succeeds.

## 7. C++ pre-VM envelope

Before a VM exists, C++ reads only the facts represented by
`umbraflow-cpp-envelope-v1.schema.json`:

```text
PreVmFacts {
    schema_version,
    content_hash,       // SHA-256 of exact runtime-model.toml bytes
    base_resolution,
    base_dpi,
    target_ids[],
    surface_ids[]
}
```

The authored top-level fields are `schema_version`, `base_resolution`,
`base_dpi`, and the IDs in `[[target]]`/`[[surface]]`. C++ computes
`content_hash` over the exact file bytes. The pre-VM reader must:

- reject a missing or unsupported `schema_version`;
- reject missing or malformed geometry fingerprint;
- reject duplicate target or surface IDs;
- reject an empty target or surface ID;
- enforce the existing 4 MiB model-file cap;
- validate script literal names against target and surface IDs;
- avoid interpreting bindings, identity predicates, locator thresholds,
  Surface selection, or transitions.

C++ does not load annotation screenshots and does not become a second semantic
model parser. Luau owns all cross-reference and recognition semantics.

## 8. Exact implementation ownership after P0

| Package | Owns | Must consume |
|---|---|---|
| P1 runtime model/compiler | constructors, TOML parsing, semantic validation, indexes | runtime schema |
| P2 C++ envelope | pre-VM facts and name/hash checks | C++ envelope schema |
| P3 resolver/receipt | tri-state evidence, stack resolution, authorization | runtime result definitions |
| P4 graph/offline validation | declared vs observed transitions, replay, coverage | Transition and offline schemas |
| P5 Agent backend | CandidateModel, Patch, Conflict, provenance, compile/validate calls | offline and API schemas |
| P6 UI | decision queue and review actions | API schema only; no runtime internals |
| P7 fixtures | synthetic models and evidence scenarios | all frozen schemas |

P1 may add generated bindings under its own write set, but it may not change
the field or enum contract without a new architecture decision. P2 must not
add semantic validation. P5/P6 must not accept or expose old model names.

## 9. Contract-level acceptance tests

The following cases are mandatory for downstream agents:

1. A scene plus a compatible overlay resolves to an ordered two-surface stack.
2. An interrupt resolves above any scene without a project-maintained order.
3. Two equally valid surfaces return `Ambiguous`, not first-match.
4. Low-confidence OCR returns `Unknown`, not `Absent`.
5. A catch-all-like frame returns `UnknownResolution` and no receipt.
6. A shared target has multiple bindings with independent placement and action
   grants.
7. A receipt from another ticket, cycle, frame, or model hash is rejected.
8. A binding action without a declared locator or precondition fails validation.
9. An absent assertion is not generated when no assertion exists.
10. A stale CandidateModel revision cannot accept or reject a patch.
11. C++ can extract target/surface names and geometry without interpreting
    binding semantics.
12. `runtime-model.toml` contains no offline screenshot or assertion records.

## 10. Non-blocking open questions

These do not change the frozen field contract:

- the concrete template-matching algorithm and score normalization remain an
  implementation choice of the vision primitive;
- the local transport may later add server-sent progress events, but the JSON
  resource and mutation shapes remain the same;
- project-specific actions beyond click/key/scroll/drag require a future
  explicit action-kind addition, not an untyped escape hatch;
- the compiler may choose generated Luau/Python/TypeScript bindings, provided
  they are generated from these schemas and do not become new authorities.
