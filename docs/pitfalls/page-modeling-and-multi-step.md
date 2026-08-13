# Page modeling and multi-step flows

Reusable knowledge for shaping page signatures and for running more than one
action against a live target. First recorded on 2026-07-25 while annotating two
卡厄思梦境 screens (main screen → character detail → character close-up) and
chaining the two clicks that navigate between them.

## A page signature requires every anchor to hit in the *same* frame

> **Historical since 2026-08-01 (`a80ea07`)** in every tool it named. The
> workbench Preview went with the GUI, and `modules/annotation` — including
> `annotation/recognition.cpp` and `SimilarityThreshold::maximumSad` — was
> deleted with the C++ page model. Deciding artifact:
> [the archived script-owned page model plan](../archive/plans/2026-07-31-script-owned-page-model.md).
>
> **What still transfers:** the conjunction itself, unchanged. The entry below is
> restated against what holds it now — `observe.resolve_page`
> (`modules/task/runtime/observe.luau`) over a page built by `Page.new`
> (`modules/task/runtime/model.luau`), and `umbra-flow check`
> (`entry/cli/check.cpp`) offline.

### Symptom

A page never resolves. `observe.resolve_page` answers with a sentence instead of
a receipt on every frame until the wait around it expires, and no click is
delivered — even though each anchor is known-good and matches its own captured
screen exactly. Offline, `umbra-flow check` reports an `unresolved_page` finding
against every screen that declares the page.

### Root cause

A page signature is strictly conjunctive: it resolves only if **every** required
identify row matches and **no** forbidden one does, all evaluated against one
frame (`observe.resolve_page`, which stops at the first clause that fails, so
rejecting a page costs one search rather than all of them). Only references
exercising `identify` are consulted; the rest of the page describes what a script
may do once it is here.

The natural authoring mistake is to treat a page as a folder of related markers.
After capturing a second screen and adding its anchor, it is easy to give that
anchor an `identify = "required"` row on the page that already exists instead of
declaring a new page. The result is one page requiring two anchors that live on
two different screens, so no frame can ever satisfy it.

The model cannot reject this: a page requiring two anchors is perfectly legal and
is exactly how a screen identified by two simultaneous markers is expressed. Only
the author knows the two markers never co-occur.

### Fix

One page per screen. Each page's required identify rows hold the anchors that are
visible **together** on that screen. Moving an anchor out is dropping its row from
the old page and declaring a new page that names it.

`identify = "forbidden"` is for exclusivity between pages whose anchors could
co-occur, not for grouping. It is only needed when two pages could both be
candidates on one frame — measure before adding it (below), because a page that
forbids an anchor it never sees costs a search per observation for nothing.

### Regression check

`umbra-flow check`: every screen that declares a page must have that page resolve
on it, which is the matrix's `unresolved_page` rule. Offline, the matcher can be
replicated in a few lines to check a whole model without running anything — the
formulas are small and fixed:

- grayscale: `gray = (77*R + 150*G + 29*B) >> 8`
  (`BgraImage::grayAt`, `modules/vision/source/vision/bgra-image.cpp`)
- ceiling: `maximum = width * height * 255`
  (`matchTemplateOnFrame`, `modules/vision/source/vision/template-match.cpp`)
- threshold: an appearance is matched when
  `score * 10000 <= maximum * (10000 - threshold)`, the basis-point test written
  multiplied out so no division rounds it (`observe.matched`)

Slide each appearance's template over every captured screen within the element's
`rect` (or the row's `rect_override`) and compare the minimum SAD against that
threshold. An anchor must hit its own screen and miss all the others. In the
recorded run the cross-screen misses sat at 2.85x–4.15x the threshold, which
settled the "do these two top-left anchors need a forbidden row to stay
unambiguous?" question with a measurement instead of a guess — they did not.

## Every step must re-resolve the page it expects to be standing on

> **Mechanism updated 2026-07-29** (was: "Multi-step navigation is a chain of
> `umbra-flow run` invocations"). The shell chain this entry originally described
> no longer exists: `--page` / `--action` and the single-step smoke flow they
> drove were deleted
> (`docs/plans/2026-07-29-three-layer-task-system.md` section 16, commit
> `e387453`), and `--timeout` / `--poll` went with the wait loop (`d1a0685`) —
> all four are now refused as unknown arguments. A multi-step flow is one
> `--task NAME` Luau script. **The lesson is unchanged and is why the entry is
> kept**: it is only self-verifying if every step re-resolves the page before it
> clicks.

### Symptom

Not a failure — a property to preserve. The engine has no loop at all: every verb
is single-shot, and `EngineSession::act` invalidates the observation it was
authorized by. So "navigate, then click the next thing" is never one operation;
it is always observe → resolve → find → click, again, from scratch.

### How

Each step waits for the page it expects, and only clicks inside that wait's
block:

```lua
return task.define {
    run = function(ctx)
        ctx:step("open_character", function()
            -- The block receives the observation CYCLE that resolved the page,
            -- so the find and the click both read the same frame.
            ctx:wait_for_page(uf.pages.main, { timeout_ms = 30000 }, function(cycle)
                local hit = cycle:find(uf.elements.battleCharacter)
                if hit then cycle:click(hit) end
            end)
        end)

        ctx:step("open_closeup", function()
            ctx:wait_for_page(uf.pages.character, { timeout_ms = 30000 }, function(cycle)
                local hit = cycle:find(uf.elements.meiling)
                if hit then cycle:click(hit) end
            end)
        end)
    end,
}
```

`ctx:wait_for_page` re-observes and re-resolves on every turn, so no step can act
on a stale belief about where it is. Step N+1 waiting for the page step N was
supposed to navigate to is what makes the chain self-verifying: if step N's click
did not land, step N+1's wait expires and raises a Tier B `timeout` rather than
clicking blind. Nothing needs to be inserted between steps — a `ctx:settle` only
avoids spending the first observations on a transition animation.

Two things the script buys that the old shell chain could not:

- **Conditionals.** `ctx:retry({ attempts = 3, on = { uf.errors.timeout } }, fn)`
  around a step, branches, and loops are ordinary Luau now. Note the strict
  reading of `on`: when it is present, a kind it does not name is **not**
  retried, even if that kind is retryable by default.
- **Popups during a wait.** An interrupt declared with `task.interrupt{ when =
  uf.pages.some_popup, ... }` is offered every resolved page on every turn of
  every wait, so a dialog that appears mid-wait is dismissed and the wait
  continues. That was the capability gap the old engine-side poll loop could not
  close.

The defaults, if you name none, are 600000 ms for `timeout_ms` and 500 ms for
`poll_ms` (`modules/task/runtime/ctx.luau`). They are policy and live in Luau
deliberately — there is no host-side fallback behind them.

### Regression check

Real-machine (2026-07-25, release build, production 750 ms lease, recorded when
this was still a two-invocation shell chain; the evidence about page modeling is
what carries over, not the invocation shape): the two steps navigated main screen
→ character detail → character close-up.
Step 1 traced 11 `PageUnknown` frames while the screen was still settling, then
`PageResolved` → `ActionFound sadScore=9780/maximumSad=299880` → `ActionAuthorized`
→ `ClickDelivered (1469,558)` → `ObservationInvalidated`. Step 2 resolved
`page_1` on its **first** frame with `sadScore=0`, then clicked (513,287). Step 2
resolving at all is the proof that step 1's click navigated; a failed step 1
leaves step 2 polling until timeout.

Note the two SAD scores: the anchor authored from a still matched its own source
at 0, while the live main screen matched at 9780 — 3.3 % of the 9000 bp budget.
Real frames drift from the captured still (highlights, counters, animation), so a
threshold that only passes at ~0 is over-fitted; the margin is the number to
watch.

## `ctx:try` catches automation failures only — your own `error()` goes straight through

### Symptom

A step body raises a plain Luau `error("...")`, the author wrapped it in
`ctx:try`, and the run still fails. `ctx:try` returned nothing — it never
returned at all, because the error went past it.

### Root cause

`ctx:try(fn)` returns `(false, err)` for exactly one class of value: a Tier B
automation error, which is host-minted userdata carrying the host's error tag.
Its test is both halves of that sentence:

```lua
if type(err) == "userdata" and getmetatable(err) == errorTag then
    return false, err
end
error(err, 0)     -- everything else is re-raised unchanged
```

A project script cannot produce such a value — `setmetatable` and `table.clone`
take tables, and `newproxy` is removed from both environments — so anything a
script raises itself is, by construction, in the `error(err, 0)` branch. That
includes a string, a table, and the Tier C cancellation sentinel (also a plain
string).

This is deliberate, and it is the reason `try` exists rather than an oversight:
a project's own bug is not an automation failure. Folding both into one return
value would make a misspelled field name look like a retryable timeout, and would
let a script swallow a cancel. `ctx:retry` uses the same test, so a retry loop
can never turn a cancelled run into three more attempts either.

### Fix

- To catch an automation failure (a wait that expired, a stale observation, a
  rejected click): `ctx:try`, and read `err.kind` against `uf.errors.<kind>`.
- To catch your **own** raise: a bare `pcall`. `ctx:try` will not do it.
- To let a bug fail the run, which is usually right: write neither.

Note that neither one is a way to keep running after a cancel. Every primitive
checks the host's terminal latch on entry, so a script that swallowed the
sentinel is refused at its next primitive call, before any capture or click.

### Regression check

`tests/task/test-framework-context.cpp`, "ctx:step nests strictly and leaves no
step open behind a raise", asserts both halves against each other in one script:
a bare `pcall` catches `error('boom', 0)` while `ctx:try` catches the Tier B
raise from a primitive.

## Re-pointing an action target at another page: authorize before withdrawing

> **Historical since 2026-07-31.** Both halves of this entry are unreachable now.
> The properties-panel checkboxes were archived with the workbench GUI
> (`b57b67b`), and the invariant they tripped over — "an action target must
> authorize at least one page" — no longer exists: `allowed_page_ids` was
> deleted, and a page's `PageReference` to an element, exercising `interact`, IS
> the authorization. Re-pointing is now adding one reference row and removing
> another; there is no lower bound to widen past, and `retypeRecognizer` was
> deleted along with the three-way type it rewrote. Deciding artifact:
> [the capability plan](../archive/plans/2026-07-31-annotation-model-capabilities.md)
> §2.2 推论 3 and §四之二.1.
>
> **What still transfers:** the ordering rule itself — *for any invariant with a
> lower bound, widen then narrow* — and the observation that a cross-field change
> no ordering can express has to be one transaction. Both outlive the fields that
> taught them.

### Symptom

Unchecking an action target's current page in the properties panel is refused
with `action_target element must authorize at least one page`, and the
checkbox snaps back.

### Root cause

Each checkbox is its own committed edit, and an action target may never hold an
empty authorization set. Withdrawing the only page produces an illegal
intermediate document even though the intended end state is legal.

### Fix

Check the new page **first**, then uncheck the old one. The intermediate state
authorizes both, which is legal. The same ordering rule applies to any invariant
with a lower bound: widen, then narrow.

The one cross-field change that no ordering can express — turning a page anchor
into an action target — is handled as a single transaction by
`retypeRecognizer` (`entry/workbench/authoring-edit.cpp`) rather than by ordering
widget edits; see the workbench authoring UI pitfalls.

### Regression check

`test-workbench` covers the transactional retype and its refusals. Real-machine:
the recorded session re-pointed `meiling` from `page` to `page_1` in that order
without a rejection (`authorized page "page_1"` then
`withdrew authorization on page "page"` in `workbench.log`).

## A colour-keyed template needs a much tighter threshold than an opaque one

### Symptom

A colour-keyed anchor matches its own page at a near-perfect score and also
matches a frame that does not contain the UI at all. Measured on 卡厄思梦境,
2026-07-30, at the default 9000 basis points: the real page scored 0–7 while a
frame with no HUD in it scored 16601 against a 70686 ceiling — a comfortable
false positive.

### Root cause

The mask decides *which* pixels SAD compares; it does not make SAD compare
*shape*. A menu glyph keyed on white selects only pixels that are themselves
~255, so the comparison reduces to "are these 416 positions bright" and any
sufficiently uniform bright region of the artwork satisfies it. The shape
information lives in which pixels the mask selects, and nothing penalises the
unselected pixels for being wrong.

This is inherent to masked matching on a monochrome glyph, not a defect in the
mask.

### Fix

Set the threshold from the measured separation rather than taking the default.
Real matches and false positives are orders of magnitude apart, so this is easy
once the numbers exist: here, real 0–7 against false 16601 means 9900 basis
points separates them with a thousandfold margin, and re-authoring the same
seven labels at 9900 gave 7/7 on an unseen frame and 0/7 on a UI-free one.

`k_defaultSimilarityBasisPoints` is 9000, which was calibrated for opaque
templates where the whole rectangle carries evidence. It is too loose for a
colour-keyed template and a masked element should pass `--min-similarity-bp`
explicitly.

### Regression check

Every colour-keyed element needs both halves: a positive frame it was not
authored against, and a frame that genuinely lacks the UI. The negative half is
what catches this, and it has to be *genuinely* UI-free — a frame captured while
the HUD was fading in still contains most of the glyph and will match, which
looks like a false positive but is not one.

## A page that resolves is not the page in front of you when an overlay is up

> Recorded 2026-08-01, driving one 出擊 run of 卡厄思梦境 page by page from a
> layer-three dispatcher. The four entries below all come from that session; how
> to choose the elements they act on is
> [element choice and thresholds](element-choice-and-thresholds.md).

### Symptom

A dispatcher that resolves a page and then does the one thing that page wants
pressed the same button **eight times** in a row.

An in-camp confirmation floats over the camp. The camp's own prompt stays
visible underneath it, so the camp page went on resolving — correctly — and the
dispatcher went on taking the camp's branch, whose action the confirmation was
covering.

### Root cause

A page signature is a conjunction over the marks that identify that page, never
an inventory of everything on its screen. So resolution answers "are this page's
marks present", which is not the same question as "is this page the frontmost
thing". An overlay that leaves the base page's marks visible leaves the base page
resolving, and both answers are right.

### Fix

Order the candidates a dispatcher walks so that **overlays are tried before the
pages they can cover**. The first page that resolves wins the step, so the order
is part of the answer rather than a performance detail.

### Regression check

With the overlay up, resolving the base page must still succeed — that is the
model being correct, not a defect to fix — while the dispatcher's walk must
select the overlay. A walk that selects the base page under an overlay is the
failure, and it is invisible in any test that resolves one page at a time.

## Order the pages a dispatcher tries by how specific their anchor is

### Symptom

A screen that has a page of its own is claimed by a different page, and the
dispatcher acts on the wrong branch.

### Root cause

The walk stops at the first page that resolves, so a page whose anchor the game
draws on several screens matches a larger set than the pages anchored on their
own text. Asked early, it swallows them.

Ordering by how common a screen is — the intuitive order, because it is the fast
one — has nothing to do with this. Frequency does not bound what an anchor
matches.

### Fix

Order by anchor specificity: overlays first (above), then every page anchored on
a sentence only it prints, then pages anchored on a shared mark **last**, where
they only catch what nothing else claimed.

### Regression check

Move a shared-anchor page to the front of the order and require a screen
belonging to a sentence-anchored page to be misreported. If nothing changes, the
anchors are already disjoint and the ordering is carrying no weight — which is
worth knowing too.

## Counting a collection is not evidence that an action happened

### Symptom

Three successful actions were logged as refusals, and the loop retried each of
them.

The check was the size of the hand before and after playing a card. The card
played deals a card as it resolves, so the hand is the same size afterwards, and
"nothing changed" was read as "the click did not land".

### Root cause

A collection's size is evidence about the collection, not about the action. Any
action whose effect replaces what it consumed is invisible to it, and the failure
direction is the dangerous one: a successful action reported as refused is
retried, so the run does the thing twice.

### Fix

Compare the count of the **name**, not the size of the collection. The card that
was selected is one fewer after it is played, whatever else arrived.

### Regression check

Test against an action that replaces what it consumes. A check that still passes
when the collection's size is held constant is measuring the action; one that
fails was measuring the collection.

## A guard needs its own evidence

### Symptom

A key pressed as a "cancel" opened a menu containing an abandon-run button.

ESC cancels a card selection. With **nothing** selected it opens an information
panel instead, and that panel offers abandoning the run. The script pressed ESC
on the belief that it had something selected.

### Root cause

The key's meaning depends on state the script never read. "I have not seen
anything go wrong" is not evidence that a key is safe to press; it is the absence
of evidence, and it reads identically in the state where the key is destructive.

### Fix

Guard on a positive reading of the state that makes the key mean what you want.
Here the screen prints a keyboard legend **exactly while a card is up**, so
reading that legend is the guard: no legend, no ESC.

### Regression check

Exercise the guard in the state where the key is *not* safe and require it to
refuse to press. A guard proven only in the state where the action was harmless
has not been proven at all.
