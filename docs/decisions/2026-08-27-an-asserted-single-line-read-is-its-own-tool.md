# 2026-08-27 — An asserted single-line read is its own Tool

## Decision

**`framework.screen.read_single_line` reads one rectangle of a retained
screenshot as exactly one line, and the caller's assertion that the rectangle
holds one line is carried by the NAME OF THE TOOL IT CALLED.** It runs no line
detection, answers at most once, and returns `text_found` with — when that is
true — `text` and `confidence`.

`framework.screen.read_lines` is untouched. It keeps Block layout, keeps its
recorded reason for keeping it, and gains no argument.

## Why this is a second Tool and not a `layout` argument

The service already carried the argument against the argument, and it is still
true:

> Block layout and never the caller's choice: this Tool exists for the region
> nobody can draw a rectangle inside, so a layout argument would offer the
> caller the one answer it came here to avoid.

A `layout` member would turn one Tool with one purpose into two behaviours
behind one name and one description. A caller reading that description would
have to be told both stories at once, and the sentence above — the reason
`read_lines` exists — would stop being true of the Tool that carries it.
Different behaviour is a different Tool. The argument contract is shared by
construction (`argumentMaterial(k_screenshotRectangleMembers)` builds both), so
a `layout` member is refused on BOTH Tools, and a test pins that.

The framework's own recorded reasoning is satisfied rather than contradicted.
`modules/task/runtime/model.luau` says of a declared read:

> `layout` has no default and is never inferred. Under `single_line` the Host
> runs no line detection at all, so a rectangle that in fact holds several lines
> comes back as one run of nonsense rather than failing; only whoever drew the
> rectangle knows which of the two it is, and a default here would answer that
> question on their behalf.

That rules out a *default*. It does not rule out an *assertion* — it demands
one. A Tool name is the only place a flat call can carry it, and it is the place
an agent reads first.

## What the Surface gate was protecting, precisely

A declared Readout is gated on its Surface resolving, and it is worth being
exact about which failure that gate prevents, because it is not the one the new
Tool's warning is about.

- **What the gate answers:** *is this the screen the rectangle was drawn on?*
  The Surface's identity Bindings — template Locators, never text predicates
  ([2026-08-17](2026-08-17-text-predicates-and-surface-identity.md)) — prove the
  frame is the one the author measured. Without it, a rectangle is coordinates,
  and coordinates read whatever now happens to sit under them.
- **What the gate does not answer:** *does this rectangle hold one line?*
  Layout is DECLARED on both sides of the gate and verified on neither. A
  Readout carrying `layout = "single_line"` reaches
  `TaskContext::cycleRead(ticket, rect, ocr::TextLayout::SingleLine)` through
  `runtime_read` in `modules/task/source/task/ffi/uf-tables.cpp`, which is the
  same function this Tool reaches. Identical code, identical reading, identical
  nonsense on a stacked rectangle.

So the capability is not new and the framework was not withholding it. What was
missing was a *route* to it for a rectangle no Surface covers — and the
motivating case has no Surface at all, because the frame exists only while a
pointer is held.

**And the gate was already absent from this whole family.** `read_lines`,
`probe`, `census_grid` and `crop` all take a raw rectangle over a raw digest
with no Surface between them. The new Tool joins an existing ungated family; it
does not open one.

**What IS newly given up, stated plainly.** Under Block, a rectangle holding
three lines answers with three entries, and a caller can count them. Under
single line that signal is gone: one answer, always, with no count and no boxes.
That is a real loss of evidence and it is exactly what the declaration must warn
about — which is why the warning is in the declaration and not in a document.
It is also a signal that was never reliable in the direction that matters: the
case that motivated this Tool is one where Block's detector fails to find a
large, legible digit at all, or finds it and reads it wrongly at a middling
score.

## Confidence is not the protection, and must not be sold as one

**No.** A recogniser handed a strip that is really two stacked lines emits a
plausible sequence of glyphs and scores its own certainty about those glyphs. It
is not scoring the caller's claim about layout, and it has no way to. A garbled
run can and does carry a high confidence.

This is why the answer here is a declaration that says so in the words a caller
will read, and not a `confidence_floor` argument. A floor would also be the
framework deciding on a Project's behalf, which it does not do
([2026-08-23](2026-08-23-the-framework-enforces-limits-it-was-given.md)): a
declared Readout carries the Project's own `confidence_floor`, and a raw Tool
hands back the number for the caller to compare.

## Whether the Tool should refuse a rectangle by size or shape

**No new refusal, and one already exists that is honest.**

A size or aspect-ratio rule would be a guess wearing a rule's clothes, and the
first thing it would refuse is the case that motivated the Tool: a cost digit's
rectangle is roughly square, or taller than it is wide. "One line" is a fact
about ink, not about a rectangle — a 4K title is over a hundred pixels tall, a
CJK column is tall and thin, one digit is a box. There is no threshold that is
right, and a wrong one would refuse correct calls while still admitting the
stacked rectangle the warning is about, because a three-line strip is usually
smaller than a large single-line label.

What already exists is a bound on **cost**, not on meaning:
`engine::k_maximumSingleLineReadPixels` is 1600×200 pixels, an eighth of a
1600×900 screen, and it refuses "read the whole screen as one line" by
arithmetic before any inference runs. It is honest precisely because it does not
claim to know what a line is. Note the direction: this Tool is bounded to
320,000 pixels where `read_lines` is bounded to 4096×4096, so the new Tool is
*more* constrained in area than the ungated one that already shipped. The
declaration promises that refusal, so
`tests/engine/test-session.cpp` now pins it where the number lives, including
that the same rectangle is admitted under Block — which is what makes the
refusal the single-line ceiling and not a rectangle the frame cannot hold.

## Replay determinism

Unchanged, and nothing about OCR made it interesting.
`ToolRuntimeExecutor::invoke` calls `replayToolCall` **before** the provider, and
a row already in a terminal state is returned as it stands. An OCR reading is
nondeterministic at the source and deterministic on replay for the same reason
`framework.workflow.now` is: the reading lands in one call's terminal outcome,
and a restart that reaches that coordinate is answered from the ledger without
the reader running again. This is the same property `read_lines` has and the new
Tool inherits it by being an ordinary Tool.

## What this costs an Operator, which a `layout` argument would not have

An Operator's policy artifact names its `privileged_surface_tools`
**individually**. Both reading Tools are Privileged, so an Operator that has
already granted `framework.screen.read_lines` does **not** thereby grant
`framework.screen.read_single_line`; a policy that does not name it refuses the
call with `Operator policy grants no Privileged surface to tool
framework.screen.read_single_line`. That was measured, not predicted — the first
run of the new case failed on exactly that sentence.

This is the honest price of the ruling, and it is the strongest thing that could
have been said for the `layout` argument: an argument rides an existing grant.
It is nevertheless the right price. A grant is the Operator's statement about
what a session may do, and a Tool whose answer discards the evidence that a
rectangle held more than the caller claimed is a different thing to permit than
one that reports what it found. Silently widening an existing grant to cover it
would be the framework deciding a policy question on the Operator's behalf.

## Rejected

- **A `layout` argument on `framework.screen.read_lines`.** Above.
- **`framework.screen.read_line`, singular.** One character apart from
  `read_lines`, with different semantics, in a catalog whose whole purpose is to
  be handed to a model that has never seen this repository. The framework
  already spells this layout `single_line` — in `model.luau`'s enum, in
  `layoutName()`, in `k_readLayouts` — so `read_single_line` also gives one word
  to one thing.
- **A `confidence_floor` argument.** The framework does not decide what score a
  Project should trust. Returning the number is the capability; comparing it is
  the Project's.
- **A `normalization` argument (`raw` / `trim` / `collapse_whitespace`).** That
  vocabulary belongs to a declared read in the RuntimeModel. A second home for
  it here would be two places one rule lives, and a Luau caller can trim.
- **Echoing the rectangle back as `x`, `y`, `width`, `height`.** Under single
  line nothing is located, so the only box available is the one the caller drew.
  `read_lines` reports what the DETECTOR measured; repeating the question here
  would look like the same kind of fact and be none.
- **Refusing a rectangle above some size or aspect ratio as a "one line" check.**
  Above.
- **Answering an empty reading as `text = ""`.** A reader that looked and saw
  nothing and a reading that came back with no characters in it are two
  different facts, and the engine already keeps them apart. `text_found`
  follows `framework.session.get`'s `present`.

## Consequences

- The Framework Tool catalog declares twenty-nine Tools and
  `tool_catalog_hash` moves. Nothing outside this repository pins that value.
- The Luau form needs no new source anywhere: the generated `@umbraflow/screen`
  module renders whatever the pinned catalog names, so
  `screen.read_single_line{...}` is callable because the catalog says so.
- `ProductLifecycle::Impl::readToolRectangle` is now the one seam both reading
  Tools open their screenshot and admit their rectangle through. The layout is a
  parameter of that seam and never an argument out at the wire, which is the
  ruling expressed in the code rather than restated beside it.
- An Operator that wants the new Tool must name it in
  `privileged_surface_tools`. Adding a Privileged Framework Tool is therefore
  never silently reachable, and that property is what the first failing run
  demonstrated.
