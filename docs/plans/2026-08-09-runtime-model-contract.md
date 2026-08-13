# Runtime model contract

> Rewritten 2026-08-14 for Runtime v2. The normative field authority is
> [`schema/umbraflow-runtime-v2.schema.json`](../../schema/umbraflow-runtime-v2.schema.json);
> this document explains that schema and the behavior implemented by the trusted
> compiler and resolver. It defines no compatibility spelling for an earlier
> model.

## Authority and boundary

`RuntimeModel` is immutable project data. C++ verifies the RuntimeArtifact's
confined file and digest closure, then one trusted Luau compiler parses the TOML
and enforces semantic references. Project code does not receive a second parser
or a way to replace model authority at runtime.

The model describes UI semantics. It does not contain policy decisions,
observations presented as declarations, controller coordinates supplied by a
project, or annotation screenshots. Runtime results are evidence-bound values
from one live cycle.

Every object is closed. Identifiers use the schema's one canonical spelling,
asset paths live below `assets/`, rectangles are `[x, y, width, height]`, points
are `[x, y]`, and sizes are `[width, height]`. The model declares
`schema_version = 2`, `base_resolution`, `base_dpi`, and the collections of
UiTargets, Locators, Readers, Bindings, optional Collections, Surfaces and
Transitions.

## Declarations

### UiTarget, Locator and Reader

A `UiTarget` is semantic identity only: `{ id, kind }`, where `kind` is
`control` or `region`. It has no placement and grants no action.

A `Locator` is a named template detector with `{ id, kind = "template",
asset_path, threshold }`. The threshold is in `[0, 1]`.

A text `Reader` declares `{ id, kind = "text", confidence_floor, layout,
normalization }`. `layout` is required and is either:

- `single_line`: read the declared rectangle as exactly one line, without line
  detection;
- `block`: locate and read every detected line in the rectangle.

There is no inferred or default layout. `normalization` is `raw`, `trim`, or
`collapse_whitespace`.

### Predicates and detectors

A detector predicate is either `locator_present` naming one Locator or
`text_equals` naming one Reader and an exact value. A detector has the three
lists `all`, `any`, and `none`; at least `all` or `any` is non-empty. These
three lists belong to a Binding variant's detector. They are not a Surface
identity language.

### Binding

A `Binding` connects one UiTarget to one Surface and owns its actionable
placement:

```text
Binding {
  id, surface, ui_target,
  placement = { kind = "fixed", rect, action_point? },
  variants = [ { name, detector }, ... ],
  actions = [ ... ],
  reads? = [ reader_id, ... ]
}
```

`variants` is non-empty. There is no top-level `variant` member. A Binding is
present only when exactly one variant detector is satisfied. No satisfied
variant means the Binding is not present or remains Unknown according to its
evidence; two or more satisfied variants produce an ambiguity and declaration
order never breaks the tie. Variant names are unique within their Binding.

A Binding action is either a click or a key action. A click uses the fixed
placement's `action_point`; it cannot carry an item-relative offset. A key
action carries its key and cannot use an action point. A placement has an
`action_point` exactly when a click needs one.

`reads`, when present, names reporting Readers. Those reads do not decide the
Binding's detector. Most Bindings report nothing and omit or leave this list
empty.

### Surface

A `Surface` declares `{ id, kind, covers, identity }`. Its kind is `scene`,
`overlay`, or `interrupt`; `covers` names the lower Surfaces with which it may
form a stack.

`identity` is one non-empty, duplicate-free list of required Binding ids. A
Surface is present when every listed Binding is present. There is no
`all`/`any`/`none` object at this level and no negative identity form. Visual
alternatives are Binding variants, not Surface disjunctions.

### Collection

A `Collection` is an ordered variable-cardinality set of items on one Surface:

```text
Collection {
  id, surface,
  placement = {
    kind = "detected", search_rect, reader,
    order = "left_to_right" | "top_to_bottom",
    slots = { origin, pitch, tolerance }
  },
  actions = [ ... ],
  reads = [ { reader, offset = [dx, dy], size = [width, height] }, ... ]
}
```

The placement Reader is detector evidence and must have `layout = "block"`.
Each detected line supplies an item's exact image-space rectangle. `order`
sorts those rectangles into stable zero-based indices; overlapping spans on the
ordering axis are ambiguous rather than inherited from detector enumeration.
The declared regular slot layout uses the one-item `origin`, an even positive
`pitch`, and a maximum assignment `tolerance`.

The detected count and item rectangles are runtime results, not authored item
geometry. A resolved Collection reports `completeness` as `complete`, `partial`,
or `unknown` and always reports `count`. `complete` and `partial` carry items;
`partial` may have gaps in slot indices. `unknown` carries no items. An absent
detector read is a complete collection of count zero.

Collection `reads` are reporting-only. Each read rectangle is derived from one
measured item origin by adding the signed `offset` and then applying the
declared absolute `size`; the size is not a delta. A rectangle leaving the
frame is refused, never clamped.

Collections own actions. A Collection click carries an origin-relative offset
and is resolved for one item selected by zero-based index. Binding clicks and
Collection clicks therefore have one spelling each: fixed `action_point` for a
Binding, item-relative `offset` for a Collection.

### Transition

A declared `Transition` has `{ id, from_surfaces, trigger, to_surfaces }`.
`from_surfaces` and `to_surfaces` are non-empty declared Surface stacks. Its
trigger names an action and exactly one owner: either `binding` or `collection`,
never both. A Collection item's index is supplied when that action is resolved;
it is not part of the declaration.

Declared destinations remain immutable model policy. An observation is stored
outside `RuntimeModel` as `{ kind = "observed_transition", transition,
observed_to_surfaces }`. Offline comparison returns both the declared and
observed destinations plus `matches_policy`; it does not rewrite the declared
Transition.

## Resolution and reported values

Resolution has two stages. `resolve_state` evaluates Surface identities and
returns one `resolved_state`, `unknown_state`, or `ambiguous_state`.
`resolve_binding` then resolves a named UiTarget, optionally constrained to a
specific Binding, only against a resolved state from the same live cycle. State
resolution never smuggles an actionable Binding into its result.

A resolved state carries a stable `id`, its `ordered_surface_stack`, and the
evidence used. An unknown state carries `reason` and evidence and may also carry
`diagnostic`; visible content matching no Surface uses that optional diagnostic
while other unknown cases remain explained by their reason. An ambiguous state
carries candidates, conflicts and evidence.

A resolved Binding carries state identity, Surface, UiTarget, Binding, selected
variant and evidence. An unresolved Binding is either `unknown_binding` with a
reason or `ambiguous_binding` with at least two candidates. Authorization of a
resolved Binding produces a `receipt_request` bound to that same state,
Binding, variant, action and evidence ids. A key request also carries the key;
a click request does not.

Every reported reading is a list of lines under both Reader layouts. Each line
is `{ rect, text }`: a block read uses the detector's measured image-space
rectangle, while a single-line read reports one element using the Binding's
declared rectangle. There is no second scalar single-line shape. A Binding
reading is `read`, `absent`, or `unknown`; only `read` has `lines`, and only
`unknown` has an `unknown_reason`. Reported readings omit the confidence score
after the Reader's floor has judged it, but retain line rectangles because
geometry participates in the decision basis.

## Confirmation and still-open behavior packages

This document records current ownership; describing a package here does not
close it.

- `T-005` owns confirmation versus recognition. The one `resolve_state` API
  confirms a caller-supplied expected Surface stack first. It returns that
  stack only when every member confirms present with no Unknown or ambiguity;
  otherwise the same call escalates to full Surface resolution. There is no
  fallback API, interrupt-only escalation path, or second recognition entry
  point. `T-005` remains owned by the consumer repository's canonical
  execution plan.
- `T-006` owns the Runtime v2 map verbs: atomic `drag(start, offset)`,
  connectivity reading with stitched-map evaluation, and conditional
  same-kind enumeration, each with its runtime and conformance gate. This prose
  does not claim those verbs are complete.
- `T-007` owns the remaining map rulings: wheel authorization, drag duration,
  colour-key ownership, and the conditional-enumeration falsifier. This prose
  does not choose those answers.
- `T-009` owns whether a distinct “there is text here” capability is still
  required now that Readers and reading outcomes exist. This document does not
  infer that capability from `text_equals`, `read`, or `absent` and does not
  close the question.

The consumer repository's execution plan is the only unfinished-work ledger.
The packages above are pointers to that owner, not duplicate work rows here.
