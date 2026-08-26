# Runtime model contract

The normative field authority is
[`schema/umbraflow-runtime-v4.schema.json`](../../schema/umbraflow-runtime-v4.schema.json).
This document explains that schema and the behavior implemented by the trusted
compiler and resolver. It defines no compatibility spelling for an earlier
model.

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
`schema_version = 4`, `base_resolution`, `base_dpi`, and the collections of
UiTargets, Locators, Bindings, Collections, Readouts, Surfaces and Transitions.
Every one of those collections may be omitted, and an omitted list is the empty
list: a row with nothing to say under a list member says it by leaving the
member out, and requiring `= []` would put a field on every row to record its
own absence. The members that must hold something -- a Binding's `variants`, a
Surface's `identity`, a Transition's endpoints -- are refused for being empty by
the rule that wanted them non-empty.

## Declarations

### UiTarget and Locator

A `UiTarget` is semantic identity only: `{ id, kind }`, where `kind` is
`control` or `region`. It has no placement and grants no action.

A `Locator` is a named template detector with `{ id, kind = "template",
asset_path, threshold }`. The threshold is in `[0, 1]`.

### How a rectangle is read

There is no free-floating Reader record. Every site that reads a rectangle
states, on itself, the three things that describe that read:

- `layout`, either `single_line` (read the rectangle as exactly one line,
  without line detection) or `block` (locate and read every detected line in
  it). There is no inferred or default layout: under `single_line` a rectangle
  that in fact holds several lines comes back as one run of nonsense rather
  than failing, so only whoever drew the rectangle can answer it.
- `confidence_floor`, in `[0, 1]`. It judges the whole set of lines: one line
  below it makes the reading `low_confidence` rather than making that line
  disappear.
- `normalization`, one of `raw`, `trim`, or `collapse_whitespace`.

Three sites read: a `Readout`, a `Collection`'s per-item read, and a
`Collection`'s detection. Detection states only its floor -- its layout is
`block` by construction, because a read whose job is to find out how many lines
a region holds cannot be told there is one, and its normalization would
describe text nothing consumes, since detection produces line RECTANGLES and
every text a Collection reports comes from a per-item read.

### Predicates and detectors

A detector predicate is only `locator_present` naming one Locator. A detector
has the three lists `all`, `any`, and `none`; at least `all` or `any` is
non-empty. These three lists belong to a Binding variant's detector. They are
not a Surface identity language.

`text_equals` remains a Collection filter only. It names one of that
Collection's own reads by that read's id, and can select among members whose
existence and rectangles the Collection's detection has already established. It
cannot establish a Binding or Surface identity.

#### Text evidence does not establish surface identity

The rejected alternative was a predicate meaning "a read found text here",
motivated by variable event-card titles. Detected Collections already
answer that question: line detection establishes the members and their
rectangles, then an optional text predicate filters those already-existing
members. Text is evidence about a Surface, never the identity of one.

The parser makes that ruling mechanical. `detector.all`, `detector.any`, and
`detector.none` refer directly to `locator_predicate`; a Binding detector that
spells `text_equals` is rejected, while the same predicate remains valid under
a Collection. The former OCR identity fixture was not retained as an exception.
The reading boundary keeps three outcomes after the Surface resolves from
locator evidence: `absent`, `read` carrying the original text, and `unknown`
carrying `low_confidence`. The schema, parser, fixtures, examples and annotation
compiler have one spelling; there is no OCR-identity exception.

### Binding

A `Binding` connects one UiTarget to one Surface and owns its actionable
placement:

```text
Binding {
  id, surface, ui_target,
  placement = { kind = "fixed", rect, action_point? },
  variants = [ { name, detector }, ... ],
  actions? = [ ... ]
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

A Binding does not read. It measures: its rectangle holds the ink a variant's
detector matched, and a Binding is present only with positive measured evidence.
A fixed rectangle whose TEXT is wanted is a Readout, declared below, which needs
no template of its own.

### Readout

A `Readout` declares `{ id, surface, rect, layout, confidence_floor,
normalization }`: one fixed rectangle on one Surface whose text is reported
whenever that Surface is on the resolved stack.

Its Surface is the whole of its gate, and it carries no detector because it
cannot: a readout region is where a value the target rewrites every frame is
printed, so there is no stable ink in it to match and a template drawn over one
would stop matching the moment the value changed. What proves the rectangle is
the rectangle is the Surface's own identity Bindings, which were measured
present to put that Surface on the stack.

A Readout attaches to no UiTarget. A centred message band inside a confirmation
popup is a thing a project has to read and is not a control anybody acts on.

### Surface

A `Surface` declares `{ id, covers?, identity }`. `covers` names the lower
Surfaces with which it may form a stack, and it is the whole of where a Surface
sits: covering nothing is the bottom of a stack, covering something is being
layered over it. There is no `kind` beside it -- a kind and a covers list were
one fact in two spellings whose agreement had to be enforced in both directions,
and that spelling also made a full-screen popup, one that dims everything under
it so nothing underneath still resolves, undeclarable except by calling it a
scene and lying about what it covers.

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
    kind = "detected", search_rect, confidence_floor,
    order = "left_to_right" | "top_to_bottom",
    slots = { origin, pitch, extent, tolerance, maximum_slots }
  },
  actions? = [ ... ],
  reads? = [ { id, offset = [dx, dy], size = [width, height],
               layout, confidence_floor, normalization }, ... ],
  predicate? = { kind = "text_equals", read, value }
}
```

The placement's detection is the evidence for the items. Each detected line
supplies an item's exact image-space rectangle. `order`
sorts those rectangles into stable zero-based indices; overlapping spans on the
ordering axis are ambiguous rather than inherited from detector enumeration.
The declared slot layout uses the one-item `origin`, a positive `pitch`, a
positive `extent`, and a maximum assignment `tolerance`. A layout of `n` items
spans `min(pitch * (n - 1), extent)`, so the declared pitch holds while the fan
is short and the spacing shrinks once `extent` binds; each slot is that centred
coordinate rounded half up, which is what lands every cardinality on integer
pixels and why `pitch` carries no parity constraint. `extent` is required
rather than optional because a Surface is finite: every centred layout already
has a bound, and omitting it would declare an unbounded fan that cannot exist.
Whether one cardinality's slots are far enough apart to assign an item to
exactly one of them is a property of that cardinality, so the resolver refuses a
detected cardinality whose smallest adjacent slot gap is below
`2 * tolerance + 1`, naming that cardinality, the gap and the tolerance.

`maximum_slots` is the largest number of slots the collection can hold, and it
is where the search for a larger layout with missing slots stops. It is required
and is declared rather than derived: `origin`, `pitch`, `extent` and `tolerance`
are all pixels, so a slot count computed from them would be a cardinality bound
the framework invented over a declaration that never mentioned cardinality. A
detected item set the search cannot place at any cardinality up to the bound is
refused by name, reporting the measured count and the declared maximum, instead
of resolving as a partial of some far larger layout.

The detected count and item rectangles are runtime results, not authored item
geometry. A resolved Collection reports `completeness` as `complete`, `partial`,
or `unknown` and always reports `count`. `complete` and `partial` carry items;
`partial` may have gaps in slot indices. `unknown` carries no items. An absent
detector read is a complete collection of count zero. A search that instead ran
out of cardinalities at `maximum_slots` resolves no Collection at all: the
declaration and the frame disagree about how large this collection can get, and
that is a named refusal rather than an unknown fit.

Collection `reads` are reporting-only. Each read rectangle is derived from one
measured item origin by adding the signed `offset` and then applying the
declared absolute `size`; the size is not a delta. A rectangle leaving the
frame is refused, never clamped. Each read's `id` is unique inside its
Collection and is the name its reported reading carries, and a `predicate` can
name only one of those sibling ids.

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

A resolved state reports one reading per Readout on every Surface of its stack,
ordered by readout id. Each reading is `{ id, kind }` plus `lines` when it read
and `reason` when it could not, where `id` is the id of the site that declared
the read -- the Readout's own, or, inside a Collection item, the per-item read's
own. There is no `ui_target` member: a UiTarget-shaped name was one a Collection
had to fill with its own id, which told a consumer a UiTarget that never existed.

Every reported reading is a list of lines under both layouts. Each line is
`{ rect, text }`: a block read uses the detector's measured image-space
rectangle, while a single_line read reports one element using the declared
rectangle. There is no second scalar single-line shape. A reading is `read`,
`absent`, or `unknown`; only `read` has `lines`, and only `unknown` has an
`unknown_reason`. Reported readings omit the confidence score after the declared
floor has judged it, but retain line rectangles because geometry participates in
the decision basis.

### Confirmation and recognition

The one `resolve_state` operation first confirms a caller-supplied expected
Surface stack. It returns that stack only when every member confirms present
with no Unknown or ambiguity; otherwise the same operation escalates to full
Surface resolution. There is no fallback API, interrupt-only escalation path,
or second recognition entry point.

### Map operations

Runtime map operations have one spelling each: atomic `drag(start, offset)`,
connectivity reading with stitched-map evaluation, and conditional same-kind
enumeration. Wheel input requires authorization, drag duration is part of the
authorized operation, colour keys remain owned by the model, and conditional
enumeration fails closed when its condition cannot be established.
