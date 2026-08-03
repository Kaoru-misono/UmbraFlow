# Element choice and thresholds

Reusable knowledge for deciding **what to annotate** and **what number to give
it**, recorded on 2026-08-01 while an agent annotated one 出擊 run of 卡厄思梦境
page by page — about 27 pages — through the exploration channel. Almost every
failure of that session had a single shape, and it is the first entry below.

Instances that stop being true when the target changes — which of *that game's*
rectangles never hold still, how long *its* UI fades in — live with the project
that observed them, in `E:\umbraflow-projects\chaos-daily\PITFALLS.md`. What
belongs here is the rule those instances taught.

Related: [colour-key annotation](colour-key-annotation.md) is the same physics
one level down, once you have decided to key a glyph;
[page modeling and multi-step flows](page-modeling-and-multi-step.md) is what to
do with the elements once they exist.

## Whether a template can work is decided by how much of its crop the glyph occupies

### Symptom

Two failures that read as unrelated and are the same one.

An element scores well on the screen it was cut from *and* on screens that do not
contain it, so no threshold separates the two. Or an element drawn to tell
several states apart scores nearly the same on all of them, so it distinguishes
nothing while looking like the healthiest element in the project.

### Root cause

SAD compares every pixel of the crop. The glyph is the part that carries the
element's identity; every other pixel in the crop is background, and it votes
with exactly the same weight. The score is therefore a blend of two questions —
did the glyph match, and does the background still look like it did on the day
the template was cut — and the blend is dominated by whichever of the two covers
more of the rectangle.

So two properties decide the outcome before any threshold is chosen: **what
fraction of the crop the glyph occupies**, and **whether what surrounds it holds
still**. Both are measurable, and both were measured. The elements that worked
fill their crop over a background that does not move (mean pixel change over
1.5 s, and the score each one matched at):

| element | mean change | match |
|---|---|---|
| the eight-pointed star at a rest point | 0.00 | 10000 bp, live |
| the filter rail on the roster page | 0.00 | score 0 |
| the centre cross on the deploy page | 0.01 | score 201 of 2652000 |

The elements that failed break one of the two properties or both:

- **The crossed swords in the corner** — a semi-transparent icon over changing
  scenery, so both halves are wrong at once. Across all stored screens its worst
  TRUE positive scored 9158 and its best FALSE positive 9070: **88 basis points
  apart**, and no threshold serves both. It was replaced by a badge — a white
  glyph on near-black, filling its crop — which matches at 10000.
- **The minimap's node icons** — small bright marks on a dark grid. The lit,
  dimmed and EMPTY slots scored **8885 / 8549 / 8582**. An EMPTY slot scoring
  8582 against a lit node's 8885 is the disease at its purest: the crop is
  mostly grid, so the grid is what matched, on all three.
- **The branch banners on the node map** — large, saturated, filling their crop.
  The battle banner scores **9297-9998** where it belongs and **8451-8557** where
  it does not, which is room to put a threshold in.

Note that the two failing cases are not near-misses to be tuned. Both scored
*high* everywhere. A high score on a screen that lacks the element is the
signature of a crop whose background did the matching.

### Fix

Before drawing the rectangle, ask two questions and answer each with a
measurement rather than a look:

1. **Does the glyph fill this rectangle?** If it occupies a small part of it,
   narrow the rectangle onto the glyph, or key the glyph so the comparison stops
   reading the background at all — a colour key is precisely the step that turns
   "glyph against whole crop" into "glyph against glyph"
   ([colour-key annotation](colour-key-annotation.md) carries the mask-size
   rules, including the opposite failure where the mask takes most of the
   rectangle) — or pick a different feature.
2. **Does everything else inside the rectangle hold still?** Two frames about
   1.5 s apart and the mean change over the rectangle answer it. 0.00 is a
   template anchor. Moving scenery is not, at any threshold.

**Replacing the feature is usually cheaper than tuning the number.** The crossed
swords produced a full table of scores and a threshold nobody could defend — 9158
against 9070 admits no answer. The badge that replaced them needed no table,
because it matches at 10000.

Where nothing on the screen fills a crop over a still background, stop looking
for a template: read the region's text instead, and let the element identify by
what it reads.

### Regression check

For any element meant to distinguish states, score it against a screen of every
state, *including every state it must reject*, and require the worst true
positive to beat the best false positive by a margin worth betting a run on. 88
basis points is not such a margin. `umbra-flow check` produces exactly these
numbers, which is why the numbers above exist; the two entries below are about
what it can and cannot see.

## A threshold set from the falsification matrix alone misses on the first live frame

### Symptom

The matrix reports a comfortable separation, a threshold is set just under the
measured true positive, and the very first live observation misses.

Measured on the 極限 switch: **9993** on a stored screen, **9401** live. A
threshold placed from the stored number failed immediately.

### Root cause

The matrix replays frozen screenshots. A screenshot froze one phase of whatever
animation runs behind the element, and replaying it a hundred times measures that
one phase a hundred times. Animation-phase variance is, by construction, outside
what the matrix can see — this is not a defect in it.

### Fix

Two measurements, and neither substitutes for the other:

- **The matrix proves "will not misrecognise."** It is the only thing that scores
  an element against screens it must reject, and it is the reason the entry above
  has numbers at all.
- **Live sampling over several frames proves "will not fail to recognise."** Open
  a few cycles against the running target, match the element on each, and take
  the *worst* score, not the first.

Set the threshold from the worst live true positive and the best matrix false
positive together. Where the two leave no gap, the element is wrong (see the
entry above); do not split the difference.

### Regression check

For any element whose background moves at all, record both numbers beside it. If
only the stored number exists, the threshold has not been measured against the
half of reality that makes it fail.

## Loosening a threshold to cure a live miss buys a misfire somewhere else

### Symptom

A live miss is diagnosed, the element's threshold is lowered to cover it, and
nothing appears to break — because the screens where it now over-matches are not
in front of you.

Measured: the crossed swords were lowered from 9400 to 9000 to fix a live miss.
The element then matched a screen whose expectation says it is absent; the matrix
reported `misfire` and the change was reverted.

### Root cause

A threshold is one number balancing two failures that pull in opposite
directions. Moving it to cure one always moves it toward the other, and the
second failure shows up on screens the person tuning is not looking at.

### Fix

Every threshold change is a model change: re-run the whole matrix, not the one
cell that motivated it. When the matrix reports a misfire, the threshold was not
the problem — the element was (first entry).

### Regression check

This is the falsification matrix earning its place: a threshold edit that a
human would have called harmless came back `misfire` on a screen nobody was
looking at. Any workflow that lets a threshold change ship without a matrix run
has given that up.

## Every appearance is another search, and a wide search region makes that cost the lease

### Symptom

A find that returns correct answers in isolation makes the step around it fail
with a stale observation. Measured: a two-appearance element searched over a
300x850 region outran even a ten-second action-frame lease.

### Root cause

`find` folds across every appearance of an element — it searches each one and
keeps the best — so the cost is the number of appearances times the area of the
search region. Both factors are chosen at annotation time and neither is visible
at the call site.

### Fix

Splitting the element into one element per type — each with its own small
rectangle — fixed it, because the cost is dominated by the area, not by the
count. The rule that transfers:

> **Small rectangles can afford appearances. Large search regions cannot.**

Note the tension this creates with the model's own ruling that mutually exclusive
states on one rectangle are ONE element with a named appearance list. That ruling
is affordable exactly while the search region is small. It is recorded as an open
cost in `docs/TODO.md`.

### Regression check

Time a find over its real search region before building a step on it. An element
whose fold does not fit inside the action-frame lease is not usable at that
rectangle, whatever it scores.

## Two buttons that print the same word are two elements

> **Narrowed 2026-08-03 (`cef4886`).** A rectangle may now come from the element,
> the row that references it, or the claim, so one element can legitimately carry
> many rectangles (`CONTEXT.md`). The rule below is therefore about an element
> that draws its OWN rectangle; the multi-placement case is expressed by a
> per-claim `rect` instead. The confusion net narrowed with it — see the
> Regression check.

### Symptom

An element authored for one button starts matching, or clicking, a different
button that happens to print the same word on another screen.

Measured: 獲得 and 離開 on the result page occupy different rectangles, and so do
the battle result's 離開 and the camp's. A rectangle drawn tight around one of
them catches part of the other's ornament.

### Root cause

The word is not the element; the rectangle is. Two buttons printing one word are
two positions, two ornaments, two surrounding backgrounds — everything a template
or a read region is made of differs, and only the text agrees.

### Fix

One element per rectangle, named for where it is rather than for what it says,
**for an element that draws its own rectangle**. Where one shape appears in
several places, the element stays one and each placement supplies the rectangle —
on the row or on the claim. That is the same ruling seen from the other side:
nine confirm buttons are one element and nine rectangles, not nine elements.

Text is what a page's rows check *after* the page is known, not what makes two
rectangles one element.

### Regression check

The matrix catches two screens claiming the same text at the **same** rectangle:
`reading.confusions` keys by (element, rectangle, text)
(`modules/task/runtime/reading.luau`) and `regress` reports it as
`ambiguous_text`, unless both screens declare the same page. A finding here means
one region is claimed to read one text on two screens the file does not declare
to be one page, so nothing resting on that region can tell them apart.

Two claims at two **different** rectangles are two keys and are never reported —
that is the rule declining to fire on one element placed twice, and it is also
its blind spot. An element that draws no rectangle of its own and is claimed at a
different rectangle on each screen gets no confusion report at all, so score it
against both screens and read the separation yourself.

## A rectangle drawn around variable-count content is wrong for some count

### Symptom

A read region measured against one screen returns nothing, or returns the wrong
line, when the same screen holds a different number of things.

Measured: the hand's cards sit at y=666..694 with nine cards and y=778..815 with
seven — the fan re-centres itself. A camp that also has a shop prints a second
prompt line, which pushes the first one up.

### Root cause

Content that lays itself out from its own count has no fixed position. A
rectangle is a fixed position. Any rectangle drawn around such content is
therefore correct for the count that was on screen when it was drawn, and wrong
for at least one other count.

### Fix

Annotate the **region**, not the line: draw a rectangle wide enough to hold the
widest case, read it in block layout, and let each line report its own rectangle.
Position then comes from the frame, which is where it actually lives, instead of
from the model, which cannot know the count.

### Regression check

Author from frames of at least two different counts and require the region to
find its content in both. One count's frames cannot tell a rectangle that follows
the content from one that happens to sit on it.

## Deleting a screen from the project file means deleting its capture too

### Symptom

`umbra-flow check` refuses before it measures anything, after a screen row was
removed from the project file.

### Root cause

The matrix pairs the Nth capture in `assets/screens/` with the Nth declared
screen, in content-hash order. A file declaring one fewer screen than the
directory holds does not describe a smaller run — it describes a *different*
pairing, in which every screen after the deleted one is measured against another
screen's pixels.

### Fix

Delete the capture with the row. This is a good refusal and worth stating
precisely because it looks like an obstacle: the alternative to refusing is a run
that reports verdicts about the wrong pictures and calls them accepted.

### Regression check

Remove a screen row without its capture: `check` must refuse and name both counts.
Remove both: `check` must run.
