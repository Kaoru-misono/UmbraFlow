# Colour-key annotation

Failures in authoring a colour-keyed template. All three below share a shape: the
tool accepts the element, and the mistake surfaces either much later or never.

Measurements come from annotating 卡厄思梦境 on 2026-07-30 with
`umbra-authoring`; mean-grey spread figures are `umbra-authoring frames probe`
output over the frames named.

## A colour key that selects no pixels is accepted, then aborts at match time

### Symptom

`page create` / `page add-anchor` / `page add-target` returns `{"ok":true,...}`
and `project save` succeeds. Every later `match` of that element fails with:

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

Before saving, run `umbra-authoring frames probe` with the same rectangle and key
and read `fully_selected_pixels`. Zero means this failure. There is no code-level
guard yet: authoring-time rejection does not exist, so the probe is the only check.

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

The acceptance threshold is derived from the template's **total** pixel count and
knows nothing about the mask. `SimilarityThreshold::maximumSad` at
`modules/annotation/source/annotation/catalog.cpp:300` computes from
`templateWidth * templateHeight`, and `catalog.cpp:410` calls it with
`spec.templateRect.width()` and `.height()`. The colour key never enters the
calculation.

So a 27-pixel mask is scored against a threshold sized for 920 pixels:
`920 * 255 * 0.01 = 2346` at 9900 bp. Twenty-seven pixels of white glyph cannot
accumulate enough difference to exceed that no matter what the frame holds.

The same shape appears in milder forms. On a `116x112` end-turn button, the 100
selected pixels were the button's outer ring, identical whether the button showed
a crossed-out circle or a tick — correct for clicking, useless for asking whether
the action was available. An `80x80` unkeyed template over that same icon, added
specifically to tell the two states apart, scored 7534-11322 on one state and
9698 on the other against a 16320 threshold, hitting both.

### Fix

Check `fully_selected_pixels` from `frames probe` and treat anything under
roughly 50 as measuring nothing. Widen the rectangle, loosen the tolerance, or
pick a different feature.

The general rule this taught, which is the part worth carrying: **an element that
hits every state it is meant to distinguish is worse than no element, because it
looks green.** This is the same discipline as the repository's falsification rule
for tests — a recognizer counts only once it has been shown to miss the state it
is supposed to reject.

### Regression check

For any element whose purpose is to distinguish two states, match it against a
frame of each and require a hit on one and a miss on the other. An element that
hits both is to be deleted, not tuned.

## Unkeyed templates only match the one screen they were authored on

### Symptom

A page's recognizers all pass on every frame available, then the page stops
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
