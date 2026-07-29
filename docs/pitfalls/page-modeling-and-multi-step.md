# Page modeling and multi-step flows

Reusable knowledge for shaping page signatures in the workbench and for running
more than one action against a live target. First recorded on 2026-07-25 while
annotating two 卡厄思梦境 screens (main screen → character detail → character
close-up) and chaining the two clicks that navigate between them.

## A page signature requires every anchor to hit in the *same* frame

### Symptom

A page never resolves. The trace is `PageUnknown` on every frame until
`waitForPage` times out, and no click is delivered — even though each anchor is
known-good and matches its own captured source exactly. The workbench Preview
reports `page: Unknown` for both sources.

### Root cause

A page signature's `required` set is conjunctive: a page becomes a candidate only
if **every** required anchor hits and **no** forbidden anchor hits, evaluated
against one frame (`modules/annotation/source/annotation/recognition.cpp`, the
`candidate = candidate && p_evidence->hit()` loop).

The natural authoring mistake is to treat a page as a folder of related markers.
After capturing a second screen and adding its anchor, it is easy to give that
anchor a `Required` role on the page that already exists instead of creating a
new page — the properties panel offers the role next to every page. The result is
one page requiring two anchors that live on two different screens, so no frame
can ever satisfy it.

The workbench cannot reject this: a page requiring two anchors is perfectly legal
and is exactly how a screen identified by two simultaneous markers is expressed.
Only the author knows the two markers never co-occur.

### Fix

One page per screen. Each page's `required` set holds the anchors that are
visible **together** on that screen. Moving an anchor out is a role change to
`None` on the old page, followed by New Page with that anchor selected.

`forbidden` is for exclusivity between pages whose anchors could co-occur, not
for grouping. It is only needed when two pages could both be candidates on one
frame — measure before adding it (below), because a page that forbids an anchor
it never sees costs a search per observation for nothing.

### Regression check

In the workbench, Preview each source: every source must resolve to exactly the
page it belongs to. Offline, the matcher can be replicated in a few lines to
check a whole model without the GUI — both formulas are small and fixed:

- grayscale: `gray = (77*R + 150*G + 29*B) >> 8`
  (`modules/vision/source/vision/sad.cpp`)
- threshold: `maxSad = (10000 - bp) * 255 * w * h / 10000`
  (`SimilarityThreshold::maximumSad`, `modules/annotation/.../catalog.cpp`)

Slide each recognizer's template over every captured source within its
`search_roi` and compare the minimum SAD against `maxSad`. An anchor must hit its
own source and miss all the others. In the recorded run the cross-screen misses
sat at 2.85x–4.15x the threshold, which settled the "do these two top-left
anchors need `forbidden` to stay unambiguous?" question with a measurement
instead of a guess — they did not.

## Multi-step navigation is a chain of `umbra-flow run` invocations

> **Superseded 2026-07-29**: `--page` / `--action` and the single-step smoke flow
> they drove were deleted (`docs/plans/2026-07-29-three-layer-task-system.md`
> section 16, commit `e387453`). The shell chain below no longer parses -- both
> flags are now refused as unknown arguments. A multi-step flow is one
> `--task NAME` Luau script run through `task::TaskHost`. The modeling insight
> this entry records -- each step re-observes and re-resolves the page it expects
> to be standing on, so a step that did not navigate fail-closes instead of
> clicking blind -- carries over to the script's page waits unchanged, and is why
> the entry is kept rather than deleted.

### Symptom

Not a failure. The engine deliberately has no loop: `EngineSession::act`
invalidates the observation it was authorized by, and `umbra-flow run` performs
exactly one `waitForPage → findAction → act` before exiting. Scripted
multi-step flows are the interim answer until the Luau engine (P0-B) lands.

### How

One invocation per step, each naming the page it expects to be standing on:

```
umbra-flow run --project DIR --selector TITLE --page page   --action battleCharacter
umbra-flow run --project DIR --selector TITLE --page page_1 --action meiling
```

Each invocation re-discovers the window, re-observes, and re-resolves the page
before it will click, so no step can act on a stale belief about where it is.
Step N+1's `waitForPage` polls (`--timeout`, default 30 s) for the page step N
was supposed to navigate to, which makes the chain self-verifying: if step N's
click did not land, step N+1 times out and fail-closes rather than clicking
blind. Nothing needs to be inserted between steps — a settle delay only avoids
spending the first observations on a transition animation.

What the chain does not give you is anything conditional: retries, branches, and
loops have to live in the shell until the Luau capability API exists.

### Regression check

Real-machine (2026-07-25, release build, production 750 ms lease): the two-step
chain above navigated main screen → character detail → character close-up.
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

## Re-pointing an action target at another page: authorize before withdrawing

### Symptom

Unchecking an action target's current page in the properties panel is refused
with `action_target recognizer must authorize at least one page`, and the
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
