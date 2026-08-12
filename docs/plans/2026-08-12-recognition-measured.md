# Recognition, measured

2026-08-12. A measurement, not a design. It answers one question with numbers
from the real game: **can this system recognise it today, and does the weighted
matching the project layer specifies need to be built?**

Everything below was produced offline against material that already exists: the
76-capture corpus at `assets/screens/`, the deployed `runtime/artifact/`
RuntimeModel, and the 70,507,282-byte `event-content-graph.json`. No live game,
no plugin, no change to the deployed project.

The short answer is in [§6](#6-is-exact-matching-enough). The weighted design
should not be built, and neither should the exact-match rule as specified —
because on real pixels three of its four components are either unreadable, wrong,
or unobservable, and the one thing that *is* on screen is not in the key.

---

## 1. What was driven, and how

The framework's own resolver was driven, not re-implemented. Per capture:

1. `TaskHostTestAccess::activate` loads the project's real `runtime/artifact/`
   directory and verifies its manifest root hash.
2. `conformance::ObservationRuntime` decodes the stored PNG into the Bgra8
   `Frame` a live capture produces and builds a real `EngineSession` over it
   through `IFrameSource` (`TargetWorld::Recorded`).
3. `TaskHost::observe` runs `modules/task/runtime/observe.luau` and
   `resolution.luau` inside the Host's trusted Luau VM and returns its own
   canonical JCS `StateResolution` document.
4. A second Host runs one extra trusted chunk that reports the same resolution
   binding by binding, and calls `resolve_binding` for all ten UiTargets when a
   surface resolved.

**`umbra-flow explore` was not usable for this and cannot be.** `explore.luau`
is an authoring surface over a *live* capture cycle: `explore_cycle_open` /
`explore_crop` bind to a target window, and `explore.probe` takes a blob but
performs pixel probing, not surface resolution. `explore` never calls
`resolve_state`. The offline seam that does exist is `engine::IFrameSource`,
which `conformance/observation-fixture.hpp` already uses for exactly this
purpose. So driving the real resolver over a stored PNG *is* possible, it is
supported by a first-party fixture, and it needs no live desktop — 76 captures,
152 Host activations, **25 seconds wall clock**.

Harness (scratch, outside both repositories):
`…/scratchpad/recognition-measured/harness/{CMakeLists.txt,main.cpp}`, built into
`build/recognition-measured/` (gitignored).

### 1.1 One thing stopped it first, and it is worth recording

The first run refused every capture with

```
runtime model schema is not supported by this trusted parser
```

The deployed artifact manifest declares
`runtime_model_schema_hash = 44be8ecf…`. The framework's **working tree** said
`a2d14589…` when this ran — an in-flight change to
`schema/umbraflow-runtime-v2.schema.json` and `runtime-model-file.hpp`. At the
`HEAD` this was measured against the constant was still `44be8ecf…`. The pin has
moved twice more on this branch since, most recently to `1f3cf1ec…` when
`$defs/unknown_reason` gained `budget_exhausted`, so treat the value as a moving
target and read it from the file rather than from here.

So: **the deployed uf-chaos RuntimeArtifact does not load against the framework's
current working tree, and will need republishing when that change lands.** The
measurement was taken against a pristine `HEAD` copy of the framework, restored
file by file with `git show HEAD:<path>` into scratch. Nothing in the framework
was written.

---

## 2. Stage 1 — does the surface resolve at all

### 2.1 What is being asked of what

| | |
|---|---|
| Captures | **76**, every one 1600×900 RGBA8 |
| Model `base_resolution` | 1600×900 — extent agrees on all 76 |
| Model contents | 2 Surfaces, 14 Bindings, 9 Locators, 10 UiTargets, **0 Readers** |
| Distinct pages in the corpus | **53** (from `legacy/page-model.toml`, which labels 74 of the 76 captures with a screen name and page; 2 are unlabelled) |

The first number is arithmetic, before any pixel is compared: a model with two
Surfaces is asked to name 53 pages. At most 3 captures — 2 `event`, 1 `recruit` —
can possibly resolve. 73 must fail, and failing is correct behaviour.

### 2.2 The distribution

| Outcome | Count |
|---|---|
| `resolved_state` → `["event"]` | **2** |
| `resolved_state` → `["recruit"]` | **1** |
| `unknown_state`, reason `no_scene_candidate` | **73** |
| `ambiguous_state` (any conflict kind) | **0** |
| `unknown_state`, reason `unknown_scene_competitor` | **0** |
| extent disagreement | **0** |
| Host or activation error | **0** |
| **Total** | **76** |

Against the legacy labels this is **exact**: the three captures that resolved are
precisely the three labelled `event`, `event_node` and `recruit`, each to the
right surface. **Zero false positives across 73 other pages, zero false
negatives.** As far as surface identity goes, this model is not weak — it is
narrow, and correct within its width.

### 2.3 "Resolution below threshold" is not a reason the system can give

The task asks failures to be split into *no binding matched* / *several matched*
/ *extent disagreed* / *below threshold*. The resolver can only produce the first
three. `evidence.absent` carries **no confidence**, so a Locator that scored
0.9407 against a 0.95 threshold and one that scored 0.49 are recorded
identically, and both roll up to `no_scene_candidate`. All 73 failures therefore
report the same reason and the fourth bucket is empty **by construction, not by
measurement**.

To fill it, a side channel calls the framework's own matcher
(`decodeTemplateImage` + `matchTemplateOnFrame`) over the same decoded frame and
the same Binding rectangles, recovering `confidence = 1 − sad ⁄ maxSad`. This is
the same computation `runtime_match` performs; only the reporting differs.

### 2.4 Every identity Locator fires on the wrong page. The conjunction is what saves it

Scores over the 73 captures that are *not* the Locator's surface:

| Locator | Threshold | Fires on wrong pages | Which |
|---|---|---|---|
| `event.battle-idle` | 0.90 | **5 / 73** | run_outcome, run_summary, signal_armed, signal_node, tail_confirm_step |
| `event.speed-x1` | 0.94 | 0 / 73 | — |
| `event.speed-x2` | 0.94 | 0 / 73 | — |
| `event.speed-x3` | 0.91 | **2 / 73** | node_map, rest_done |
| `recruit.filter-badge` | 0.95 | 0 / 73 | — |
| `recruit.confirm-check` | 0.95 | **10 / 73** | card_reward, card_reward_grid, duplicate_assign, equip_assign_mythic, equip_pick, equip_pick_chaos, fate_choice, fate_transfer, fighter_list_assigned, flash_assign |

`recruit.confirm-check` reaches **1.0000** on `flash_assign` — a perfect match on
a page that is not recruit. But no capture satisfies *both* halves of either
Surface identity: `battle-idle ∧ any speed` → 0 wrong pages,
`filter-badge ∧ confirm-check` → 0 wrong pages.

The flat `all` / `any` conjunction the RuntimeModel v2 SurfaceIdentity allows is
therefore **the only thing standing between this model and 15 false positives**.
That is a property worth protecting, and it is the strongest positive result in
this document.

---

## 3. Stage 2 — the rule cannot be applied to a capture, for four separate reasons

The rule under test:

> page type + normalised event title + visible option count + the current map or
> encounter pool → exactly one candidate, otherwise Unknown.

Applied to the three captures that resolved, it fails before it starts. The four
reasons are independent; fixing any one leaves the other three.

### 3.1 The model declares no Reader, so the Host cannot produce a title

`runtime/artifact/page-model.toml` contains **zero `[[reader]]` entries** and no
Binding declares `reads`. `resolve_readings` therefore returns the empty list for
all three resolved captures, and the canonical document carries no `readings`
member at all. **No text of any kind reaches a caller through the supported
path.** The title component of the key has no producer.

### 3.2 Even with a Reader, the title is not on the screen

Both event captures were cropped and read. Neither shows an encounter name.
They show narrative flavour text:

| Capture | On-screen text above the options |
|---|---|
| `308a849b…` (`event`, card layout) | 自稱流浪者佩雷格林的人／似乎打算與隊伍分享自己的休息處。 |
| `4e945c4e…` (`event_node`, bubble layout) | 聖物周圍散落著破碎的機械天使。 |

The card capture is encounter `uk_evt_s04_01`, whose name in the graph is
**星之流浪者**. That string is nowhere on the page. And the string that *is* on
the page — `自稱流浪者佩雷格林的人` — **does not occur anywhere in the
70,507,282-byte graph** (0 hits; likewise 0 for the bubble capture's line). The
event page renders a field the compiled slice does not carry, and does not render
the field the key is built on.

This is a two-sided miss: the key names a field that is not displayed, and the
display shows a field that is not compiled.

### 3.3 The visible option count is wrong on the first real event capture

`resolve_binding` for `event.option_1/2/3`:

| Capture | option_1 | option_2 | option_3 | Resolver's count |
|---|---|---|---|---|
| `308a849b…` card layout | `no_binding_candidate` | `event.option-2.card` | `event.option-3.card` | **2** |
| `4e945c4e…` bubble layout | `event.option-1.bubble` | `event.option-2.bubble` | `no_binding_candidate` (no ordinal-3 bubble Binding exists) | **2** |

The card capture **shows three options**. The first is greyed out — already taken
— and its ornament therefore scores **0.8768** against a 0.95 threshold, because
the template was cropped from a bright card. The resolver is not wrong about the
pixels; the model has one Binding per ordinal per layout and no variant for a
disabled option.

For that one capture there are now **four different numbers for "option count"**:

| Source | Value |
|---|---|
| What the screen shows | 3 |
| What the resolver reports | 2 |
| `declared_option_count` in the graph | 3 |
| `resolved_option_count` in the graph | 5 |

The bubble capture genuinely shows 2 and the resolver reports 2, which is the
only place the component behaves.

### 3.4 The pool is neither on screen nor walkable

The graph holds no map, floor, pool, stage or zone **entity**. Entity types are
exactly `{encounter: 884, encounter_option: 2715, encounter_option_effect: 3055,
encounter_reward: 761, text_resource: 3427}`, and `link_encounter_pack_id` is a
scalar `opaque_relation` with no target — nothing is walkable from it. Pack ids
are internal strings (`uk_904_s01`) with no text resource and no localised form,
so nothing renders them.

**The pool must be carried in session state**: something has to observe the run
entering a pack and keep that fact alive across captures. A cold start or any
drift silently invalidates a third of the key. The project's own shipped pack
already says so — its `candidate_key.pack_source` is `project_state`.

The source data *does* contain the map→pool chain (`db/map_list.db`,
`db/map_fixed_group_encounter.db`, `db/floor_set.db`, per-namespace
`encounter_pack.db`), among 1,588 logical tables. **None of them are in the
compiled typed slice.** The grouping is recoverable by extending the compiler; it
is simply absent from this graph.

### 3.5 Stage 2 capture-side distribution

| Outcome over the 3 resolved captures | Count |
|---|---|
| resolved to exactly one candidate | **0** |
| Unknown | **3** |
| ambiguous | **0** |

All three are Unknown for want of a title, before content ambiguity is ever
reached. That is a fact about the model and the screen, not about the content.

---

## 4. What the rule *would* do, measured over the content

The capture side cannot exercise the rule, but the content side determines
whether the rule could ever work. Measured over all 884 encounters of
`event-content-graph.json` (`a940dc11…`, 70,507,282 bytes — the `c0c97ba2…` copy
is byte-identical; `d82c5f50…`/`195348d6…` differ only by one trailing newline).

Normalisation, stated once and used throughout: NFKC → strip
`U+200B U+200C U+200D U+2060 U+FEFF` → collapse whitespace runs and trim →
casefold. No alias table.

### 4.1 Inventory

| | Count |
|---|---|
| Encounters | 884 |
| With a resolved `zht` name | 859 |
| With no name at all | 25 |
| With a resolved `en` name | **0** (all 884 Missing) |
| `interpretation_status = Partial` | 550 |
| `interpretation_status = Conflict` | **334** ✔ |

The 334 `Conflict` figure reproduces. All 334 resolve **zero** options, and the
set of encounters with zero resolved options is exactly those 334. One
correction: their declared counts are `{0:1, 1:97, 2:26, 3:210}` — **one declares
0, not 1–3**. 12 of the 334 are also unnamed.

`declared ≠ resolved` for **558** of 884. Resolved counts run to 15.

### 4.2 The name is not a key — confirmed exactly

859 named encounters carry **371 distinct names**, **198 shared**, largest group
**14** (`命運見證者`, all in pack `uk_evt_s04`). Every stated figure reproduces.

Normalisation changes **nothing**: raw distinct 371, normalised distinct 371,
zero merges. On authored data it does no work at all. It is insurance against
OCR-side width and whitespace variance, and nothing more.

### 4.3 Per-component discrimination

Population: the 859 named encounters. "Unique" = alone in its equivalence class.

| Key | Classes | Unique | Ambiguous classes | Encounters in them | Max class |
|---|---|---|---|---|---|
| K1 title | 371 | 173 (20.1%) | 198 | 686 | 14 |
| K2 title + resolved count | 496 | 306 (35.6%) | 190 | 553 | 10 |
| K3 title + declared count | 459 | 255 (29.7%) | 204 | 604 | 11 |
| **K4 title + declared count + pool** | **474** | **278 (32.4%)** | **196** | **581** | **11** |
| K5 title + pool | 397 | 204 (23.7%) | 193 | 655 | 14 |
| — title + declared + `set_id` | 572 | 393 (45.8%) | 179 | 466 | 8 |

Class-size distribution under K4 (size → classes/encounters):
`1:278/278  2:89/178  3:64/192  4:24/96  5:8/40  6:6/36  7:4/28  11:1/11`.

**Which component actually discriminates:**

- **page type: 0.** The graph holds one surface. Page type can select the event
  surface; inside it, it separates nothing. It is in the key for routing, not for
  discrimination.
- **title: everything.** Alone it isolates 173 of 859 (20.1%). Nothing else comes
  near that base.
- **option count: +82** (173 → 255). The only meaningful addition. It splits 82
  of the 198 title-collision groups.
- **pool: +23** on top of title+count (255 → 278), and +31 over the bare title.
  It splits 13 of 204 groups. **766 of the 859 named encounters sit in a name
  group living entirely inside one pack**, where the pool cannot separate
  anything by construction. That is 2.7 percentage points bought with a
  session-state dependency (§3.4).
- `link_encounter_set_id` — equally unobservable — outperforms the pool on every
  measure (393 unique vs 278). **The key picks the weaker of the two available
  groupings.**

### 4.4 What the Conflict encounters do

The rule's "visible option count" has to be matched against something, and the
rule statement does not say which:

- **against `resolved_option_count`:** no Conflict encounter can ever match (the
  screen shows 1–3, the record says 0). Worse, `resolved` is in 1..3 for only 395
  of 884 and exceeds 3 for 155. **489 of 884 (55.3%) become permanently
  unmatchable.**
- **against `declared_option_count`** (what the project's own pack does): 883 of
  884 are matchable, and Conflict encounters match and then answer with a record
  that has no options behind it.

They do **not** poison anyone else. Under K4 the number of ambiguous classes
mixing Conflict with non-Conflict is **0**. Conflict encounters collide only with
each other: 78 all-Conflict ambiguous classes covering 231 encounters.

Reachability over all 884 under K4, declared reading:

| Bucket | Count |
|---|---|
| no resolved name — never keyable | 25 |
| named, ambiguous class — rule must answer Unknown | 581 |
| named, unique class, but `Conflict` | 91 |
| named, unique class, but `declared ≠ resolved` | 63 |
| **named, unique, clean and usable** | **124** |

**An exact K4 match yields a unique *and* usable encounter for 124 of 884 —
14.0%.**

### 4.5 The measurement was already in the deployed project

`content/compiled/<hash>/event-recognition-pack.json`
(`uf-chaos-event-recognition-pack/v1`) already implements this exact key —
`parts: [link_encounter_pack_id, declared_option_count, normalized_zht_name]`,
the same normalisation, exact match or no candidate. Its own `coverage` block
declares `ambiguous_key_count: 196`, `indexed_count: 859`,
`selectable_count: 325`, `withheld_by_reason {Conflict: 334, OptionCountMismatch:
558, UnresolvedOptionEdge: 334}`. The independent measurement above reproduces
its index exactly — 474 classes, 278 singletons, 196 ambiguous — which is a
cross-check that two implementations agree.

The graph-side answer was therefore already known and already written down. What
was missing, and is supplied here, is the capture side.

---

## 5. The ambiguous cases, and what would separate them

196 ambiguous K4 classes covering 581 encounters. Every "separates" verdict below
was computed across the class, not guessed. Full per-class table:
`…/scratchpad/graph/k4-residue.csv`.

How often each candidate field **fully** separates an ambiguous class:

| Field | Fully separates | Partially | Never | On screen? |
|---|---|---|---|---|
| **option flavour texts (ordered)** | **77** | 25 | 94 | **yes — the option buttons** |
| reward ids/types (ordered) | 71 | 12 | 113 | only after choosing |
| effect `overview_eff_desc` | 52 | 26 | 118 | only after choosing |
| reward `desc_option` text | 43 | 35 | 118 | maybe (tooltip) |
| option `dice_value` | 40 | 27 | 129 | maybe (dice pips) |
| option `rarity` | 40 | 34 | 122 | no |
| `link_encounter_set_id` | 33 | 50 | 113 | no |
| `resolved_option_count` | 22 | 33 | 141 | it *is* the count |
| `rarity` | 17 | 42 | 137 | no |
| `logical_namespace` | 0 | 1 | 195 | no |
| **`en` name** | **0** | **0** | **196** | zero exist |

Best available separator, one per class:

| Best separator | Classes | Encounters |
|---|---|---|
| **nothing in the graph** | **85** | **273** |
| option flavour texts | 77 | 196 |
| reward ids/types | 11 | 44 |
| `link_encounter_set_id` | 7 | 27 |
| `rarity` | 6 | 12 |
| everything else | 10 | 29 |

**The 85 unseparable classes split cleanly in two:**

- **67 classes / 204 encounters are entirely Conflict**, every member resolving
  zero options. There is nothing to compare because the compiler never resolved
  their option edges. Concentrated in `uk_403_s01` (15), `uk_402_s01` (14),
  `uk_401_s01` (13), `uk_400_s01` (11). **This is a compiler-coverage defect, not
  a key-design defect.** Resolving those edges would give 204 encounters
  something to be separated by.
- **18 classes / 69 encounters are genuine modelling collisions.** Eight of them
  (23 encounters) are **byte-identical across every field the graph holds** —
  same title, same declared count, same pack, same single option (`進入戰鬥`),
  same effects, same rewards; e.g. `鋼鐵之雨管轄卡厄思深處` ×4 in `uk_trc_s02`,
  `非法研究所地下2樓入口處` ×3 in `uk_trc_s03`. **No key, no score, no
  threshold, and no fuzzy matching will ever separate these.** They differ only
  by which map node the player stepped on — data the graph does not carry.

### 5.1 Adding the field that is actually on screen

| Key | Classes | Unique | Ambiguous classes | Encounters in them |
|---|---|---|---|---|
| K4 title + declared + pool | 474 | 278 | 196 | 581 |
| title + option-text tuple | 621 | 497 | 124 | 362 |
| **title + declared + pool + option-text tuple** | **643** | **523** | **120** | **336** |
| option-text tuple alone | 405 | 353 | 52 | 506 |

Option text nearly doubles uniqueness, 278 → 523 of 859. Of the 120 classes still
ambiguous, **78 (231 encounters) are zero-option Conflict records with an empty
tuple** — literally nothing to compare. Only **42 classes / 105 encounters have
real option text that still collides.** With option text present, the pool's
marginal value falls to **12 encounters**.

### 5.2 Positive control on the two real event captures

The option labels visible on the two event captures were matched exactly against
`encounter_option.simple_flavor_text`:

| Capture | Visible labels | Candidates | Named encounter | K4 would have given |
|---|---|---|---|---|
| `308a849b…` | 詢問有關命運的事 / 請求演奏 / 請求共鳴之曲 | **1** | `uk_evt_s04_01` 星之流浪者 | 1 (but the title is not on screen) |
| `4e945c4e…` | 嘗試啟動聖物 / 調查屍體 | **1** | `uk_900_s02_01b` | **2 — Unknown** |

The bubble capture is the whole argument in one row: the specified key leaves two
candidates and must answer Unknown, while the two strings actually rendered on
the page name exactly one encounter. Across the population the full option-label
set is unique for **581 of 884**.

One caveat found the moment real pixels were used: the bubble capture's first
option renders as **`[分析] 嘗試啟動聖物`**, a UI-added tag. Exact match on the
decorated string returns **0 candidates**; stripping the tag returns 1. A rule
with "no alias table, no normalisation" meets this on its first real capture.

---

## 6. Is exact matching enough?

**No — and the weighted version would not help, so build neither.**

Stated plainly:

1. **The rule as specified cannot be evaluated against a capture at all.** Three
   of its four components fail on real pixels: no Reader exists to produce a
   title (§3.1), the title is not rendered (§3.2), the option count is wrong on
   the first card-layout capture (§3.3), and the pool is unobservable and
   uncompiled (§3.4). Capture-side result: 0 resolved, 3 Unknown, 0 ambiguous.
2. **Over the content, exact matching on that key leaves 581 of 859 named
   encounters (67.6%) ambiguous**, and yields a unique *and usable* record for
   124 of 884 (14.0%). That number is already published in the project's own
   `event-recognition-pack.json`.
3. **Weights would not move it.** 85 of the 196 ambiguous classes (273
   encounters) are separated by **no field anywhere in the graph** — 67 classes
   because the compiler resolved no options, 18 because the rows are duplicates,
   8 of those byte-identical. A weighted scorer over fields that are equal on
   both sides returns a tie however it is tuned. Weights can only reorder
   evidence that differs; here the evidence does not differ.
4. **What does move it is changing the key, not the matcher.** The option button
   labels are on screen, are in the graph, fully separate 77 of the 196 ambiguous
   classes, and lift uniqueness from 278 to 523 of 859. Both real event captures
   resolve to exactly one encounter by option text, including one the specified
   key would have answered Unknown for.

So the finding is not "exact is too weak, add weights". It is **"the key names
the wrong fields"**. The honest ordering of what is owed, stated here and not
built:

- the RuntimeModel needs Readers before any text rule is testable — today there
  is nothing to weigh;
- the option Bindings need a disabled-option variant, or the option count is
  measured wrong whenever a choice is greyed out;
- the content compiler needs the unresolved option edges (204 encounters) and the
  map→pool tables (uncompiled today) before the pool is even a candidate
  component;
- 23 encounters in 8 classes are permanently unnameable from this data and should
  be declared so rather than chased.

---

## 7. Why this measurement is weaker than it looks

- **The event surface rests on n = 2.** 76 captures cover 53 pages, but only two
  are `event` and one is `recruit`. Every capture-side claim about option layout,
  option count and greyed options comes from two images. §3.3's four-way count
  disagreement is one capture.
- **The corpus is unlabelled by anyone independent.** Ground truth comes from
  `legacy/page-model.toml`'s own `[[screen]]` table — the superseded model's
  authoring, not human review. It agrees with the resolver perfectly, which is
  reassuring and also exactly what a shared ancestor would produce.
- **The corpus exists on one machine.** `assets/screens/` is gitignored with no
  remote. Nothing here can be reproduced elsewhere without it.
- **The content directory moved during the measurement.** `d82c5f50…` was deleted
  and `195348d6…` appeared while this ran. The primary numbers are pinned to
  `a940dc11…` (2026-08-09, 70,507,282 bytes), which has been stable.
- **The screenshots and the graph may be different game builds.** Nothing ties a
  capture's date to a content pack version, and one on-screen string was absent
  from the graph (§3.2) — flavour text the slice does not carry is the likely
  explanation, but a build skew would look identical.
- **The side-channel scores use a hand-transcribed rect table.** The Binding
  rectangles in `main.cpp`'s probe table were copied from `page-model.toml`; the
  resolver reads the file itself. A transcription error would move §2.4 and §5's
  score tables, though not the resolver's own outcomes in §2.2.
- **Only `zht` exists.** Zero `en` names resolve, so no cross-locale check on the
  title is possible.

---

## 8. Numbers that could not be obtained, and why

| Wanted | Why not |
|---|---|
| Failure split by *below threshold* vs *no match* from the resolver | `evidence.absent` carries no confidence; all 73 failures report `no_scene_candidate`. Recovered only by a side channel outside the resolver's vocabulary (§2.3). |
| Stage 2 distribution over captures (resolved / Unknown / ambiguous-with-N) | The model declares no Reader and the title is not on screen. The distribution is 0 / 3 / 0, which measures the model, not the rule (§3.5). |
| The real on-screen option count as the graph would state it | Four different numbers exist for one capture (§3.3). Which one a capture agrees with is a capture-side fact, and n = 2. |
| Discrimination contributed by page type | Zero by construction — the graph holds one surface (§4.3). |
| Whether the pack id is ever displayed | Established only that it is not derivable *from the graph*: no entity, no text resource, no localised string. A HUD element showing a human-readable region name cannot be ruled out; the table that would map one back to a pack (`db/map_list.db`) exists in the source pack but is not compiled. |
| Whether `simple_flavor_text` is literally the button label | Verified on 2 captures by reading the pixels and matching the strings exactly (§5.2). Not verified for the other 882 encounters. |
| Timing under a real capture pipeline | The frame source replays a decoded PNG; the 25 s figure measures decode plus matching, and says nothing about WGC capture latency. |

---

## Appendix — reproducing

```
harness   …/scratchpad/recognition-measured/harness/{CMakeLists.txt,main.cpp}
build     build/recognition-measured        (gitignored; cmake -DUMBRAFLOW_ROOT=<pristine HEAD copy>)
stage 1   …/scratchpad/recognition-measured/out/stage1-full.txt      (76 observe, 565 detail, 1064 score rows)
labels    …/scratchpad/recognition-measured/out/legacy-labels.json   (74 screen→page rows from legacy/page-model.toml)
stage 2   …/scratchpad/recognition-measured/out/capture-stage2.txt
graph     …/scratchpad/graph/{encounters.csv,k4-residue.csv,class-size-distributions.csv,report.txt}
```

The framework must be built from a pristine `HEAD` copy until the in-flight
schema change lands and the project's artifact is republished (§1.1).
