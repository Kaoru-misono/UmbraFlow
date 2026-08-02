# Colour-key annotation

Failures in authoring a colour-keyed template. All three below share a shape: the
tool accepts the element, and the mistake surfaces either much later or never.

Measurements come from annotating 卡厄思梦境 on 2026-07-30 with
`umbra-authoring`; mean-grey spread figures are `umbra-authoring frames probe`
output over the frames named.

## A colour key that selects no pixels is accepted, then aborts at match time

### Symptom

`page create` / `page add` returns `{"ok":true,...}`
and `project save` succeeds. Every later `match` of that element fails with:

(Corrected 2026-07-31: the drawing verbs were `page add-anchor` / `page
add-target` / `page add-info` when this was recorded; they collapsed into
`page add ROOT PAGE NAME --capability C... <draw>` with the capability model.
The failure is unchanged — `page create` and `page add` are still the two verbs
that draw pixels, and neither checks the key.)

(Corrected again 2026-08-02: all of those verbs are gone with `umbra-authoring`,
and the verb that draws pixels is `explore.crop`, through `scribe.measure`. This
failure can no longer be authored at all — see the Regression check below for
what refuses it and why that one is a refusal where the floor below is not. The
reproduction is kept because the mechanism it names, `sad.cpp`, is unchanged.)

```
InternalInvariant: template mask excludes every pixel of its template | at sad.cpp:325
{"ok":false,"error":{"kind":"InternalInvariant","response":"abort",...}}
```

Reproduced with `--key 255,255,255 --tolerance 20` over rect `1378,18,62,58`,
which contains a grey outline icon and no white at all.

### Root cause

The check is real and correctly placed for the matcher:
`modules/vision/source/vision/sad.cpp:322-329` takes
`totalWeight = p_templateMask->weightSum()` and fails when it is zero, because a
masked score normalizes by that weight and cannot divide by nothing.

What is missing is any check at authoring time. The authoring command holds both
the source frame and the key, so it could refuse immediately and name the key and
the rectangle. Instead the element is persisted and the failure appears on a
later run, classified `InternalInvariant` — which reads as "the program is
broken" when the truth is "this key matches nothing inside that rectangle".

### Fix

Drop the key when the rectangle is already stable unkeyed, or key on the glyph's
real colour rather than the colour it looks like. In the case above the same
rectangle measured 0.152 mean-grey spread unkeyed across three frames and needed
no key; on a later frame from a different battle map it needed a key after all,
and the working one was the outline's own grey, `170,175,190` at tolerance 60,
giving 0.274 over 211 pixels.

### Regression check

Before saving, probe the same rectangle with the same key and read
`fully_selected_pixels` and `ramp_selected_pixels`. **Both** zero means this
failure — that is the sum the host refuses on and the sum `sad.cpp` aborts on,
because a template whose mask has any weight at all still matches.

> **2026-07-31: the drawing verbs now say it themselves.** `page create` and
> `page add` measure the mask they just drew and report it under
> `authored.mask`, in the same two counts `frames probe` uses and out of the same
> `probeColour` call, so the two documents cannot disagree. Zero selected pixels
> lands under the floor below and comes back with a `warning`. It is a hint and
> not a refusal — the element is still authored and `ok` stays true — so the
> probe remains worth running, and this failure is now visible at the moment it
> is made rather than at a match much later.
>
> **Superseded 2026-08-02, and the zero case is now REFUSED.** The CLI that note
> describes is gone: `umbra-authoring`, `entry/authoring/` and
> `tests/authoring/` were deleted with the v4 line on 2026-08-01, and the mask
> is measured by `explore.crop` now — the exploration primitive an agent cuts
> its template with, which is also where the key is applied. Read `frames probe`
> as `explore.probe`, and `authored.mask` as the crop's third return value:
> `key_red`, `key_green`, `key_blue`, `tolerance`, `rect_pixels`,
> `selected_pixels`, `ramp_selected_pixels`, and a `warning`. The four key
> fields are the key the HOST applied, defaults filled in, and they are what the
> authoring layer writes into the appearance — read them off the mask rather
> than off the argument that was passed.
>
> What changed besides the spelling is the verdict on THIS failure. A key that
> selects no pixel is refused where it was chosen, as an `InvalidResource`
> naming the key and the rectangle, rather than warned about. The reason is not
> that the count is more trustworthy than it was — it is that the artifact is
> not merely poor, it is unusable: the crop's mask IS the PNG's alpha channel
> now, so a key that takes nothing produces a fully transparent template, and
> `sad.cpp:322` aborts on every match of one. A verb that handed back bytes
> whose only future is an abort would be handing back garbage, and refusing
> costs the agent one retry with a different colour. The floor below stays a
> warning, for the reasons that section gives.
>
> Deciding artifact: `measureCropMask` in
> `modules/task/source/task/task-context.cpp`, and the case
> `A key that selects nothing is refused where it was chosen` in
> `tests/task/test-colour-key-crop.cpp`.

## A mask with too few selected pixels passes everything and measures nothing

This is the worse of the two, because nothing fails.

### Symptom

A keyed element matches on every frame, including frames whose content has
visibly changed, and scores near zero on all of them. It looks like the most
reliable element in the project.

Measured: a `46x20` rectangle (920 pixels) with a white key selected **27**
pixels. It scored **0** on three frames in which that rectangle had gone from
reading "開始 1" to showing a card thumbnail.

### Root cause

> **Corrected 2026-07-31.** The original entry blamed the threshold formula for
> ignoring the mask. It does not, because the *score* is rescaled to match it.
> The measurements below are unchanged and still reproduce; only the mechanism
> was wrong, and aiming a fix at the threshold would miss.

The threshold is derived from the template's total pixel count —
`SimilarityThreshold::maximumSad` (`catalog.cpp:300`) computes from
`templateWidth * templateHeight` — but the score handed to it is normalized onto
that same scale. `normalizedScore` (`modules/vision/source/vision/sad.cpp:47-62`)
returns `weightedSum * templatePixels / totalWeight`, and `sad.cpp:392` is where
the reported score goes through it.

Work the constants through and the template size cancels on both sides. The live
decision is:

> **accept iff the weighted mean grey error over the *selected* pixels is at most
> `255 * (1 - t)`** — at 9900 bp, a mean error of 2.55 grey levels.

So the threshold *is* mask-relative, and 27 white pixels over changed content
would exceed it easily. That is not what happened.

**What actually happened is the search, not the threshold.** The match is the
argmin over the whole search ROI (`sad.cpp:351-403`), with an exact-match early
exit the moment any candidate position scores zero (`sad.cpp:394-400`). Twenty-
seven saturated-white pixels do not have to survive where the glyph *was* — they
only have to find *some* offset inside the ROI where all 27 land on white. On a
card thumbnail there are many such offsets, the search finds one, scores 0, and
returns immediately.

The driver is therefore **mask size against ROI size**, not the threshold. A tiny
mask of a saturated colour is a template that asks "is there a patch of white
anywhere in this region", and the answer on a busy screen is always yes.
`docs/pitfalls/page-modeling-and-multi-step.md:246-254` states the same mechanism
for a keyed menu glyph.

The same shape appears in milder forms. On a `116x112` end-turn button, the 100
selected pixels were the button's outer ring, identical whether the button showed
a crossed-out circle or a tick — correct for clicking, useless for asking whether
the action was available. An `80x80` unkeyed template over that same icon, added
specifically to tell the two states apart, scored 7534-11322 on one state and
9698 on the other against a 16320 threshold, hitting both.

### The same failure at the other extreme, and why the count alone is the wrong test

> Added 2026-07-31, measured on `session-0731-round2\frames` with
> `umbra-authoring frames probe`.

A key that takes almost the whole rectangle fails for the same reason a tiny one
does, and it passes any pixel floor with room to spare. The masked comparison
only reads the *values* of the selected pixels, and a colour key selects pixels
within tolerance of one colour by construction — so a mask that covers most of
the rectangle is a solid patch of that colour, any patch of it the same size
matches, and the glyph-shaped holes carry no weight at all.

The `繼續進行` button, one rectangle `1268,795,315,58`, three keys:

| key | fully selected | share | verdict |
|---|---|---|---|
| the button's orange fill `250,131,50` tol 40 | 12421 of 18270 | **68.0%** | flat: no structure |
| **the white glyph `255,255,255` tol 12** | **154** | 0.8% | glyph-shaped, works |
| the white glyph, rect narrowed to the text | 30 | 0.2% | under the floor |

Selection reads the first frame given, so the middle row measures 179 rather than
154 on a different capture of the same screen. Neither number changes the verdict,
and that is the point: the two failure modes bracket the answer. The rectangle has
to stay wide enough to hold enough glyph pixels, and the key has to pick the glyph
rather than the fill. Two more of the same disease, both proposed as anchors and both
rejected: the map/node info icon `1512,124,44,44` keyed white takes 1250 of 1936
pixels (64.6%) and is one white disc, and the `確認` button in its disabled state
is 75% uniform grey.

Against those, every element in `chaos-super` that survived cross-page
falsification selects between **6.6% and 25.8%** of its rectangle — 89, 136, 152,
198, 227, 240, 251, 316, 339, 367, 386, 439, 638 and 707 pixels. A discriminating
mask is a *figure* carved out of the rectangle: big enough to constrain a search,
and small enough that what got selected is the figure rather than the ground.

### Fix

Check the fully selected count against **both** ends. Under roughly 50 it
measures nothing; at half the rectangle or more it distinguishes nothing. Widen
the rectangle, loosen the tolerance, key the glyph instead of the fill, or pick a
different feature.

> **2026-07-31: landed as a warning on the drawing verbs, not as a refusal.**
> `page create` and `page add` measure the mask and attach
> `authored.mask.warning` when the count is under 50 or the share is at or above
> half. Deciding artifact: `maskWarning` in
> `entry/authoring/command-runner.cpp`, and the case
> `the drawing verbs warn about a mask that cannot measure anything` in
> `tests/authoring/test-authoring-cli.cpp`, whose third subcase exists to keep
> the warning off the good masks above.
>
> **Moved 2026-08-02, unchanged in substance.** That CLI is deleted. The same
> two ends, the same 50 and the same half, are now measured by `explore.crop` at
> the moment it cuts the template, and reported on its third return value under
> `warning`. Deciding artifact: `maskWarning` and
> `k_minimumUsefulMaskPixels` / `k_maximumUsefulMaskShareBp` in
> `modules/task/source/task/task-context.{cpp,hpp}`, and the case
> `The crop warns about a mask that can measure nothing` in
> `tests/task/test-colour-key-crop.cpp` — whose first of three keys is the good
> mask, and exists for the same reason the old third subcase did: a hint that
> fires on a working mask says nothing.
>
> It stays a warning for exactly the two reasons given here, and the wider
> falsification matrix is still the gate. What did NOT stay a warning is the
> zero-selection case above, and the line between them is worth naming: this
> floor rejects masks that are *probably* useless, where a zero mask is one that
> *provably* cannot be matched at all.
>
> [The capability plan](../plans/2026-07-31-annotation-model-capabilities.md)
> §2.3 P0 originally wanted this floor as a construction-time refusal in
> `Appearance::create`. That was **demoted to a warning** (see the plan's
> "两条实现期裁决" B), for two reasons this section is the evidence for: a
> appearance carries a `sourceId` and no pixels, so that layer cannot count
> anything; and a pixel count is the wrong measure on its own, since it waves the
> 68% orange fill straight through. **The gate stays the falsification matrix**,
> `umbra-authoring check` — that measures whether the element hits a screen it
> must not, where these two numbers only guess from shape. (The matrix is
> `regress.check` and `umbra-flow check` now; the ruling is unchanged.)

The general rule this taught, which is the part worth carrying: **an element that
hits every state it is meant to distinguish is worse than no element, because it
looks green.** This is the same discipline as the repository's falsification rule
for tests — an element counts only once it has been shown to miss the state it
is supposed to reject.

### Regression check

For any element whose purpose is to distinguish two states, match it against a
frame of each and require a hit on one and a miss on the other. An element that
hits both is to be deleted, not tuned.

## Unkeyed templates only match the one screen they were authored on

### Symptom

A page's elements all pass on every frame available, then the page stops
resolving entirely when the same screen is reached at a different location in the
game.

Measured on the battle page, entering a second battle map: `battle_shift_label`
(a page anchor) scored 14244, `battle_shift_lock` 17235 and `battle_draw_pile`
19095, against thresholds of 3702, 9169 and 14137 — all misses. On the same
frame `battle_esc` scored 0, `battle_end_turn` 1311 and a keyed hand element 518.
Every unkeyed template on the page broke; every keyed one held.

### Root cause

Not a fading or absent label: the rectangle in question held exactly 86
near-white pixels on all four frames including the new map. The top bar those
elements sit on is not opaque, so a different background reads through it and an
unkeyed template is matching that background as much as the glyph.

The reasoning that produced the mistake was "this sits on the opaque top bar, so
it needs no key". That premise was never measured — only assumed from how the bar
looks.

A related instance on the same page: a rectangle that included a fading key-hint
pill went from matching to 386026 against an 18360 threshold on a frame caught
mid-fade, where the pill held 68 near-white pixels instead of 129. The pill's
opacity varies continuously, so no template containing it is reliable.

### Fix

Key anything drawn over game content, and narrow the rectangle to the glyph that
is actually fixed. The menu button became the hamburger glyph alone, keyed white,
measuring 0.000 over 204 pixels across five frames spanning three screens. The
currency widget became the coin icon alone — the original rectangle included the
amount, so it stopped matching when the amount changed from 100 to 124 — keyed on
the coin's gold, 0.000 over 362 pixels.

### Regression check

Author from frames of at least two different locations of the same screen, and
require every element to hit both. One location's frames cannot distinguish a
template that matches a glyph from one that matches a background.

> **2026-08-02: this mechanism now has a case in the repository.**
> `A masked template ignores the scenery an unmasked one matches on` in
> `tests/task/test-colour-key-crop.cpp` builds exactly the frame this section
> describes — one where the glyph is byte-identical and only the scenery behind
> it is repainted — and measures both spellings of one template on it. Unmasked
> it scores 20000 where it should score nothing, and worse than the 13440 it
> scores on a frame its glyph is ABSENT from: it prefers the wrong screen, which
> is how an element comes to hit every state it was meant to tell apart. Masked
> it scores 0, because none of the changed pixels carry weight.
>
> It exists because the case beside it does NOT prove this. That one changes the
> glyph and leaves the scenery alone, so the pixels that moved are already the
> only pixels the mask keeps, and a mask covering the whole rectangle would pass
> it. The two together bracket the claim: one shows the mask reaching the
> matcher, the other shows it excluding.
