# 2026-08-29 — A shape search takes the variants the caller prepared

## Decision

**`framework.screen.match_shapes` searches one rectangle of a retained
screenshot for caller-supplied Gray8 templates by exact zero-mean normalized
cross-correlation, and every scale and angle the caller wants is its own
template with its own id.** The Tool synthesizes nothing: no rotation, no
scaling, no colour gate, no OCR, no semantic class. A nonempty alpha mask is
refused rather than honoured.

It is a measuring Tool, so it opens the immutable artifact its required
`screenshot_sha256` names, captures nothing, writes nothing and clicks nothing.
Every bound it carries — at most 64 templates, each at most 64 by 64, an ROI of
at most 4,194,304 pixels, at most 65,536 raw candidates above threshold, and the
caller's own 1..512 output limit — refuses the whole call when exceeded. There
is no partial answer and no silent truncation.

## Why the caller prepares the variants

The alternative is a `scales` and `angles` argument, and it looks cheaper right
up to the point where someone has to say which resampling filter produced the
rotated template. That is not a detail: bilinear, nearest and Lanczos give
measurably different correlation scores on a 14-pixel glyph, and the difference
is larger than the margin most thresholds are set with. A Tool that resamples
has therefore made a decision on the project's behalf, and made it invisibly,
inside a call whose arguments do not mention it.

Handing the caller a `pixels` array instead makes the whole input auditable. The
bytes that were compared are the bytes the caller sent. When a match is wrong,
the template that produced it can be read back out of the call, and when a
project changes its preparation — a different filter, a native-resolution crop
instead of an upsample — nothing in the framework needs to learn about it.

This is the same line the framework holds everywhere else: it records, verifies
and enforces declared limits, and does not decide.

## Why exact NCC, and not the existing SAD matcher

`sad.cpp` already matches templates, and reusing it was the first thing tried.
It could not answer this question. Its masked path skips every excluded pixel —
an excluded pixel contributes nothing, `weight == 0` continues — so a template
cut out by colour keying scores a perfect 1.0 against any sufficiently large
plain field of the glyph's own colour. That is recorded in
`docs/pitfalls/colour-key-annotation.md`, and it is why this Tool refuses a mask
outright instead of accepting an empty one as a special case.

Zero-mean NCC scores over **all** the pixels and normalizes by both variances.
A constant patch has zero variance and is skipped rather than matched, and a
constant template is refused at validation, because neither has a correlation to
report. Positive affine brightness change — the whole patch scaled and offset —
leaves the score unmoved, which is what makes the same template usable on a
frame the game has brightened.

It is not invariant to clipping, to saturation, or to a spatially varying
additive glow, and the Tool's description does not claim it is.

## Why an exceeded bound refuses instead of truncating

A truncated result is indistinguishable from a complete one at the call
boundary. A caller that receives 512 matches when 900 survived suppression has
been handed a subset chosen by the framework, under a rule the caller never
stated, and there is no member of the answer that says so. Refusing hands back a
fact the caller can act on: the search as specified does not fit, so narrow the
rectangle, cut templates, or raise a threshold.

`completed_pixel_comparisons` is reported on the confirmed path for the same
reason — a caller sizing a region against the session's budget needs the number
the last search actually spent, not an estimate.

## Rejected

- **`scales` / `angles` arguments.** See above: the resampling filter is a
  decision, and it would live inside the Tool where no argument names it.
- **An alpha mask.** The masked SAD path exists and is exactly the failure this
  Tool was written to avoid. Accepting a mask and requiring it to be empty would
  be two spellings of one thing.
- **A colour gate, or reporting each match's dominant colour.**
  `framework.screen.probe` already measures colour in a rectangle. A second
  spelling inside this Tool would also have to choose a histogram bucketing,
  which is another decision made for the project. A caller that wants colour
  evidence for a match calls `probe` on the rectangle this Tool returned; the
  two have independent failure modes, which is the point.
- **Coarse-to-fine or FFT acceleration.** Both trade the "exact, exhaustive, no
  truncation" semantics for speed that the measured cost below does not yet
  need, and both make `completed_pixel_comparisons` mean something other than
  what it says.
- **Reporting matches as centres rather than rectangles.** A centre cannot say
  which candidates were clipped by the rectangle's edge; the returned
  `x`/`y`/`width`/`height` are absolute screenshot coordinates precisely so a
  caller can see that.

## Consequences

The default per-search budget, `k_defaultPixelComparisonBudget`, is 2^31. A
search's worst case is the number of candidate positions times the template
area, summed over templates, and that product grows fast enough that a caller
can walk into a refusal without changing anything structural.

Measured on the first consumer, over a 990 by 260 rectangle with 30 templates of
14 and 16 pixels: **1,626,757,860 comparisons, 586 ms median in a Release
build** — 76% of the default budget in one call. Two further 16-pixel templates
took it to roughly 81%. A caller that wants both a finer scale grid and a
non-trivial angle grid over a rectangle that size **cannot have both** under the
default budget; four scales by three angles over five sources is 3.47 billion,
which refuses.

The shape that fits is a smaller rectangle per expected position rather than one
rectangle over the whole region, and that is the caller's arrangement to make.
The Tool's job here is to refuse honestly and report what it spent, which it
does.

Release builds are what the timing above describes. A Debug build of the same
search is far slower and can cross the 10-second measuring timeout; that is a
property of the build, not of the search.
