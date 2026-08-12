# Project content at scale: what one `derive` actually reads

> Archived 2026-08-12: the seekable-container recommendation was refuted and
> normalized readings now reach `derive` through `ui_snapshot`. Surviving
> ceiling, conformance and consumer work is owned by
> [the consolidated outstanding plan](../../plans/2026-08-12-outstanding-work.md)
> `P-005`, `P-006` and `C-001`–`C-004`.

Status: **its measurements stand and its central recommendation was refuted by
its own numbers.** The seekable container this document proposes was **not**
built: it measured the projection at 1.55 MiB against a 4 MiB cap and then
recommended a container anyway, which is designing for a load nobody had run. A
compact canonical JSON projection shipped instead, and a later measurement in a
real Luau VM put it at 2,876,995 bytes on disk and 62.2% of the memory quota
decoded — inside every ceiling. §5 is the record of an argument, not work owed.

What did survive is the finding this document was not sent for: `derive` receives
no observed string at all, so a perfect content index has nothing to look up by.
That is being closed separately.

Every number below was measured on this machine against the real `uf-chaos`
content pack; §8 lists what could not be measured and says so.
Date: 2026-08-12
Scope: `umbraflow-cpp`. It proposes one container format, one sandbox addition
and one derive-envelope member that the framework would own, and states what a
consumer build must emit into them. No consumer file is written by this
document.
Measured against: `uf-chaos` content pack
`a940dc110bfbb52280a609a8fb3d4b05172152146a604dbfb002bf70624ca127`.

## 1. Two holes, and the second one is larger

**The stated hole.** `modules/script/source/script/ffi/pure-data-program.cpp:56-57`
caps one artifact at 4 MiB and all artifacts at 16 MiB;
`modules/operator/source/operator/project-plugin.cpp:26-27` restates the same
two numbers for the registrar. `uf-chaos`'s compiled event graph is 70,507,282
bytes. What its two deployments actually pin is `blob/dream-page-model.blob`
(35 bytes, `schema = "umbraflow-project/l2-v2"`) and
`blob/archive-content-pack.blob` (37 bytes, `{"schema":"uf-chaos-content-pack/v1"}`).
The framework's own second game does the same: `examples/arcana-expedition/blob/map.blob`
is 20 bytes, and its content is the ASCII string `expedition-map-bytes`.

`artifact.read` has never been called by a plugin. `rg` over both trees finds it
in exactly one place outside its own implementation:
`tests/operator/test-project-plugin-contract.cpp`, four call sites, and the
largest blob any of them builds is 1 MiB + 1 byte, constructed to prove that the
**output** ceiling refuses it. No plugin in `examples/`, and neither `uf-chaos`
plugin, reads an artifact at all.

**The hole under it.** `derive` has nothing to query *with*.
`k_deriveInputSchema` (`modules/deployment/source/deployment/project-deployment.cpp:128-164`)
is `additionalProperties: false` over exactly five members, and the only one
carrying the world is `ui_snapshot`, typed as `StateResolution`:

```json
{"kind": "resolved_state|ambiguous_state|unknown_state",
 "ordered_surface_stack": ["..."], "reason": "..."}
```

`modules/task/source/task/ui-observation.hpp:82-88` states the intent in the
class comment: it carries "the kind, the ordered surface stack of a resolved
state, and the failure reason of the others."

The trusted resolver does read text. `modules/task/runtime/observe.luau:88-124`
(`readText`) calls `runtime_read`, applies the reader's `normalization` and
`confidence_floor`, and produces `{kind="read", text=..., confidence=...}`. It
then uses that text *only* to test a page-model predicate —
`observe.luau:149`, `reading.text == predicate.value`. The text is dropped one
level later: `modules/task/runtime/evidence.luau:110-120`, `evidence.summary`
returns `{id, kind, reason, confidence, source}` and deliberately omits
`result.value`. So the read string never reaches the evidence summary, never
reaches the `StateResolution` document, and never reaches the plugin.

`uf-chaos-project-layer-design.md` §6.2 opens its recognition order with
"UI surface resolved -> read event group/name/narrative/option count/dice/icon/
reward shape". The plugin gets the first line and nothing after it. A perfect
content index in the plugin's hands today has no observation to look anything up
by. **This is the blocking item; the artifact ceiling is the second one.**

## 2. The query profile, measured

### 2.1 What the 70.5 MB is

The file is already minified: re-serialising the parsed document with compact
separators yields 70,507,281 bytes against a 70,507,282-byte file. Top level is
`{entities: 10842, links: 22090, report, graph_digest, schema_version}`.

| Region | Bytes | Share |
|---|---:|---:|
| `entities[]` (compact) | 52,136,234 (49.72 MiB) | 74% |
| top-level `links[]` | 18,176,192 (17.33 MiB) | 26% |
| `report` | 194,690 | 0.3% |

The top-level `links` array is a **second copy**. Its 22,090 objects are the
same multiset, compared as canonical JSON, as the 22,090 link objects embedded
in `entities[].links`. 17.33 MiB — a quarter of the file — is duplication.

Inside `entities[]`, by field:

| Field | MiB | Share of entity bytes |
|---|---:|---:|
| `links` | 17.35 | 34.9% |
| `raw_variants` | 10.15 | 20.4% |
| `source_refs` | 4.75 | 9.6% |
| `localized_text` | 4.07 | 8.2% |
| `opaque_relations` | 3.79 | 7.6% |
| `raw_fields` | 3.77 | 7.6% |
| `typed_fields` | **0.80** | **1.6%** |
| identity fields (`entity_id`, `entity_digest`, `canonical_primary_key`, `logical_namespace`, `type`, `source_native_ids`, `interpretation_status`) | 2.54 | 5.1% |

Three specific amplifications account for most of it:

- **`raw_fields` is stored twice.** 10,405 of 10,842 entities have a
  `raw_fields` byte-equal to one of their own `raw_variants[].raw_fields`. Only
  437 entities have two variants.
- **`source_refs` are stored ~7×.** 87,036 occurrences of 12,440 distinct
  source-reference objects; the distinct set is 4,955,641 bytes.
- **The `en` locale carries no English.** 5,504 `localized_text` entries for
  `en`, of which `Resolved` is 0 and `Missing` is 5,504 (the build report agrees:
  `localization_by_locale_resolution.en.Resolved = 0`). Those entries cost
  1.10 MiB, and the `.en` link records that pair with them cost a further
  4.00 MiB. 5.1 MiB conveys zero English text.

Entity census, with compact bytes:

| Type | Count | MiB | Median B | Max B |
|---|---:|---:|---:|---:|
| `encounter_option` | 2,715 | 18.60 | 7,144 | 12,086 |
| `encounter_option_effect` | 3,055 | 13.95 | 4,509 | 11,783 |
| `encounter` | 884 | 6.85 | 7,862 | 17,231 |
| `text_resource` | 3,427 | 5.42 | 1,679 | 1,919 |
| `encounter_reward` | 761 | 4.89 | 6,506 | 10,200 |

`text_resource` is the sharpest illustration. Each one exists to hold one
string. Per entity it spends 598 B on `raw_variants`, 401 B on `source_refs`,
66 B on `entity_digest` and 44 B on `typed_fields`; the identifier and the text
together are 85 B. All 3,427 texts, as `{key: text}`, are 499,726 bytes — 9% of
what the type occupies in the graph.

### 2.2 What one event recognition reads

Following §6.2's order and §6.3's `CurrentEventView` / `ObservedChoice`, one
recognition needs, for each candidate encounter: its identity and namespace, its
`interpretation_status`, its typed fields (`declared_option_count`,
`resolved_option_count`, `encounter_type_new`, `rarity`,
`link_encounter_pack_id`, `link_encounter_set_id`), its `zht` name; then per
option, in declared order, its identity, status, typed fields
(`order`, `option_type`, `select_type`, `dice_value`, `rarity`) and `zht`
flavour text; then per option effect its status, `effect_fixed`,
`nonempty_reward_slot_count` and effect descriptions; then per reward its
`reward_type`, `amount_min`, `amount_max`, `skip` and descriptions.

Taking the **full transitive closure** of one encounter over its resolved links,
with every field kept, costs:

| | entities | bytes |
|---|---:|---:|
| min | 3 | 15,482 |
| median | 14 | 64,918 |
| p95 | 27 | 148,287 |
| max | 63 | 334,684 |

Taking the **recognition projection** of the same closure — the fields listed
above and nothing else — costs:

| | bytes |
|---|---:|
| min | 665 |
| median | 1,752 |
| p95 | 3,757 |
| max | 8,831 |

**A 37× reduction, and it is the whole finding.** All 884 encounters projected
this way are 1,626,348 bytes of record bodies — **1.551 MiB**, against a 4 MiB
per-artifact cap. The entire event corpus of the game already fits, with room,
inside the ceiling that was described as the blocker.

The 70 MB is not query surface. It is the audit artifact — provenance, source
locators, raw variants, the shadowed-candidate record §4.4 requires — and it is
correct that it exists. It is wrong that it is what a plugin would be handed.

### 2.3 What the candidate key can be

§6.2 says "query candidates by run map + encounter pools + confirmed branch
history". Measured against this graph, that query cannot be executed:

- The graph has five entity types and no others: `encounter`,
  `encounter_option`, `encounter_option_effect`, `encounter_reward`,
  `text_resource`. §4.5 lists mission/stage/planet/map/floor/node and encounter
  pack/set among the relations the graph must cover; none of those are entities
  here. The build report agrees — `entities_by_type` has exactly those five keys.
- `link_encounter_pack_id` is an `opaque_relation` on all 884 encounters and
  `link_encounter_set_id` on 603. The pack is a string on the encounter, not a
  node the graph can walk from a map.

So a plugin can filter encounters by pack, but the pack it is in must come from
`ProjectState`, not from the content graph.

Candidate-set size under each key the graph does support, in projected bytes:

| Key | Buckets | Largest bucket | Median bucket |
|---|---:|---:|---:|
| `resolved_option_count` | 12 | 334 enc / 673,246 B | 62,556 B |
| (`option_count`, `encounter_type_new`) | 30 | 237 enc / 579,695 B | 13,626 B |
| `logical_namespace` | 10 | 250 enc / 609,954 B | 116,798 B |
| `link_encounter_pack_id` | 51 | 58 enc / 158,077 B | 12,313 B |
| (pack, `option_count`) | 126 | 58 enc / 158,077 B | 3,892 B |

**Name is not a key.** 859 of 884 encounters have a `Resolved` zht name, but
they carry only 371 distinct normalised names; 198 of those names belong to more
than one encounter and one name is shared by 14. A normalised-name index over
the corpus is 23,030 bytes and answers with an ambiguity set, which is what
§6.2's "文本只能作为证据,不能作为身份" already predicted.

**One third of the corpus is unusable as content.** 334 of 884 encounters are
`interpretation_status: Conflict`, and they are exactly the 334 whose
`resolved_option_count` is 0 — while declaring 3 options (210 of them), 1 (97) or
2 (26). 1,022 `encounter.has_option` links and 1,167 `option.success_effect`
links are `VariantConflict`. This is §4.4's "同路径多实例必须显式投影" unmet
for the five paths the build report names in `typed_projection_conflict_paths`.
Recognition can address **550 encounters, 953,988 projected bytes**; the other
334 (673,246 B) can only ever return `Conflict`.

### 2.4 The ceiling that actually binds

It is not the artifact cap. Everything a read puts into the VM is charged to the
same 16 MiB `k_memoryQuotaBytes` allocator (`allocator.hpp`,
`createStateWithQuota`).

> **Superseded 2026-08-12 on the mechanism, not on the arithmetic.**
> `artifact.read` no longer pushes bytes. Every registered artifact is parsed at
> registration and a read builds the decoded value directly, frozen, once per
> VM. So the decode below is not something a plugin does inside the tick budget
> — it is what a read *is*, and its cost is the table figures in this section
> rather than those figures plus the source string. Two consequences for the
> numbers here: the "add the source string itself" line no longer applies, and
> a plugin that reads one artifact twice pays once.

Costing a decode against Luau's own object sizes (`VM/src/lobject.h`: `TValue`
16 B, `LuaNode` 32 B, table header ~72 B, `TString` header 32 B + length; and
`VM/src/lstring.cpp:148` interns **every** string regardless of length, so each
distinct string is charged once per VM):

| Decoded | tables/arrays | interned strings | total |
|---|---:|---:|---:|
| one projected record (median) | — | — | 8,432 B |
| one projected record (max) | — | — | 38,406 B |
| largest pack, 58 records, 158,077 B JSON | — | — | 0.576 MiB |
| whole projected corpus, 1.551 MiB JSON | 5.29 MiB | 0.44 MiB (9,099 distinct) | **5.73 MiB** |
| whole 70.5 MB graph | 117.9 MiB | 4.0 MiB | **121.9 MiB** |

Add the source string itself and a whole-corpus decode peaks near 7.3 MiB of a
16 MiB quota — affordable once, wasteful per call. The whole graph needs **7.6×
the quota** before a single field is read.

Two further budgets bind, both per invocation: `k_maximumRuntime` = 2 seconds,
and `k_interruptBudgetTicks` = 2,000,000 interrupt invocations. And
`ledger.cpp:3770-3785` runs `derive` **inside the `BEGIN IMMEDIATE`
transaction**, with the comment "The plugin VM is quota-bound, so holding the
write lock across it is bounded." Whatever a `derive` does to content, it does
while holding the database's write lock.

Finally, the sandbox has no JSON decoder. `k_pureGlobals`
(`pure-data-program.cpp:63-76`) whitelists 23 names —
`bit32`, `math`, `string`, `table`, `utf8` and the base functions. A plugin that
wants structure out of bytes writes a decoder in Luau, against that 2,000,000-tick
budget.

> **Closed 2026-08-12, and this paragraph is why.** The whitelist still has no
> decoder and must not grow one. What changed is that no plugin needs one:
> `artifact.read` answers with the decoded value, so an artifact carrying JSON
> is data rather than a capability nothing in the environment can express. The
> tick budget no longer bounds a hand-written scan of the pack, because there
> is no scan.

## 3. The budget, weighed

`k_maximumArtifactBytes` and `k_maximumTotalArtifactBytes` appear twice, as
duplicated literals in two translation units, with no `static_assert` and no
comment in either file saying why 4 and 16.
`docs/plans/2026-08-09-claude-handoff.md:213` records the numbers ("64 roots、单
blob 4 MiB、总计 16 MiB、输入/输出 1 MiB、错误文本 4 KiB") and not the reason.
`docs/plans/2026-08-11-consumer-attestation.md` treats them as a settled
constraint and reasons *from* them ("A directory does not fit"). So the only
recorded justification is "the VM is quota-bound", and that justification points
at `k_memoryQuotaBytes`, not at 4 or 16.

**Ruled here: the number is not what has to give; the shape is.** Three
measurements say so, in order of decisiveness.

1. Raising the artifact cap to 71 MiB would not deliver the graph. The read
   copies the blob into a 16 MiB-quota VM, so a 67.2 MiB `artifact.read` fails
   at the allocator before the plugin sees a byte. Raising the quota too means
   carrying 67 MiB of string plus 121.9 MiB of decoded tables — ~190 MiB per
   `derive`, inside a write transaction, under a 2-second ceiling. That is not a
   number adjustment; it is abandoning the reason the pure VM exists.
2. The cap is not binding for the real query. The whole event corpus that
   recognition reads is 1.551 MiB, well under the existing 4 MiB. Nothing needs
   to be raised for `uf-chaos` to ship its content today.
3. The 64-root ceiling is the one that will bite, and only under a design that
   spends roots on sharding — see option B below.

There is one number worth revisiting on its own merits, and it is not these:
`k_maximumDataBytes` = 1 MiB bounds `derive`'s **input**, which is where §6 of
this document proposes to put observed readings. That bound is fine for readings
and should stay; it is named here only so a later reader does not mistake it for
an oversight.

## 4. The options

### A — Shard by access key, one root per shard, with an index the plugin loads

The registration names N artifact roots; the build emits one blob per shard plus
one index blob; the plugin reads the index, decides which shard, reads that
shard.

Fit, measured: sharding by pack gives 51 shards, largest 158,077 B — under 64
roots, barely. Sharding by namespace gives 10.

*Costs the project author*: it invents and maintains a shard key; every content
addition risks crossing 64; and because root names live in the registration, the
shard **count** is frozen into `project_registration_hash`, so a content build
that produces a 52nd pack changes the registration rather than one root hash.

*Costs the framework*: nothing to build — and that is the problem. Each project
derives its own sharding, and the second game derives it again, differently.
Worse, the scheme runs out before the second game arrives: `uf-chaos` would be at
51 of 64 roots for encounters alone, with map, card, battle, shop and camp
content still unmodelled. **Rejected on the tie-break and on arithmetic.**

### B — A host-side content reader the plugin calls through a pure verb

`content.query(...)` beside `artifact.read`, answered by trusted C++.

*Costs the project author*: least work of any option — no container, no index.

*Costs the framework*: the most, and in the currency it can least afford.
`derive` stops being a pure function of its recorded input, so replay, decision
basis and §6.2's "不得读取未记录的全局历史" all break unless every query and
every answer is recorded into the observation — which is the artifact bytes
again, with extra steps. To answer a query the framework must execute one, which
means the framework interprets project content: exactly what `P-04` and the
`D-*` ownership column exist to prevent. And the sandbox has one frozen reader
by design; a second host callback is the shape this tree has repeatedly refused.
**Rejected on ownership, not on effort.**

### C — A compiled query artifact per surface

The build emits one artifact per UI surface holding exactly the corpus that
surface's recognition reads.

Fit, measured: the event surface's whole corpus is 1.551 MiB — one root, under
the cap.

*Costs the project author*: it must decide what a surface's recognition reads,
which it has to decide anyway.

*Costs the framework*: it must state the partitioning axis, which is one
sentence, because "one root per surface" is already the vocabulary the
`RuntimeModel` and `UIActionIntent` speak.

This is the right **partitioning** and an incomplete **answer**: it fixes a cap
that was not binding, and leaves the plugin decoding 1.551 MiB per `derive`
(5.73 MiB of Luau values, inside the write transaction). Keep the axis; add the
seek.

### D — Raise the ceiling

Covered in §3. It does not deliver the graph, and the graph is not what should
be delivered.

### E — A seekable record artifact, one root per surface

An index the plugin decodes in full, and record bodies it decodes one at a time.
One blob per surface, in a framework-specified container: a canonical JSON
header carrying the schema, the record count and a directory of
`(key, offset, length)`, followed by the record bodies at those offsets. The
plugin reads the blob once, decodes the header, selects candidate keys, and
`string.sub`s exactly those records.

Fit, measured on the event surface:

| Part | Bytes |
|---|---:|
| record bodies, 884 records | 1,626,348 |
| directory `(id, offset, length)` | 27,892 |
| first-pass index (id, name, option count, type, rarity, pack) | 102,220 |
| normalised-name index | 23,030 |
| one record, median / p95 / max | 1,752 / 3,757 / 8,831 |
| one record decoded into Luau, median / max | 8,432 / 38,406 |

A twenty-candidate `derive` decodes the directory plus about 170 KiB of Luau
values against a 16 MiB quota. The whole-corpus decode (5.73 MiB) remains
available and inside quota, so a plugin whose index is wrong degrades instead of
failing.

Luau's interning works in this design's favour: `luaS_newlstr` searches the
string table for every length, so reading the same blob twice in one VM costs
its bytes once, and identical record slices deduplicate.

*Costs the project author*: it emits a container instead of a document, and it
must choose record keys. Both are work it owes anyway.

*Costs the framework*: one container schema, one Luau prelude (§6.2), and the
derive-envelope change of §6.1 — which is owed regardless of which option wins.

## 5. Recommendation

**Adopt E, with C's axis: one artifact root per UI surface, each root a
framework-specified seekable record container whose bodies the framework never
interprets.**

The reason is the tie-break, stated as an arithmetic fact rather than a
preference. Under A, `uf-chaos` picks a shard key, the second game picks a
different one, and the framework learns nothing from either; under E the
framework owns the envelope — index, offsets, keying, the reader — and a project
owns only what a record *says*. That is the same split the tree already uses
three times: `opaque_project_payload` inside a typed `ExpectedEffect`,
`report_hash` beside a report the producer keeps
(`2026-08-11-consumer-attestation.md` §2), and `TrustedReplayResult` beside a
replay upstream never sees. A second game reuses the container and the reader
and writes only its own records.

The secondary reason is that E is the only option whose cost is bounded by the
*candidate set* rather than by the *corpus*. A, C and D all decode a whole shard
per `derive`, inside the write transaction. E decodes a 27 KiB directory and the
records it names.

The recommendation does **not** raise any ceiling. 1.551 MiB against 4 MiB, one
root per surface against 64.

## 6. What the framework then owes

### 6.1 Observed readings in the derive envelope — blocking, and independent of everything above

Without this, no content design can be exercised, because `derive` has nothing
to match content against (§1).

The framework already produces what is needed and discards it.
`observe.luau:88-124` normalises a reading against the page model's declared
`normalization` and rejects it below the reader's `confidence_floor`;
`evidence.luau:110-120` then drops `result.value` when summarising. What is owed:

1. `evidence.summary` carries the normalised value for `kind == "present"`
   readings, or a sibling accessor does.
2. `StateResolution` gains a bounded member carrying, per resolved binding, the
   reader id and the normalised reading — not raw OCR, not coordinates, not a
   frame. `U-04` is unaffected: this is trusted-resolver evidence, and geometry
   still proves nothing.
3. `k_deriveInputSchema` gains the corresponding member. It is
   `additionalProperties: false`, so this is a deliberate edit of a compiled-in
   string in `project-deployment.cpp` and its hash consequences, not an
   accident.
4. The readings enter the decision basis with the resolution hash that already
   binds it, so a replay sees what the plugin saw.

`P-03` needs a ruling here and gets one cheaply: it withholds raw
coordinate/OCR/input/Receipt capability from the **online Agent**. A
`ProjectPlugin` is a different principal — registered by hash, pinned into
`project_registration_hash`, running in a pure VM with no handle. Handing it
normalised, confidence-floored, reader-attributed strings is not the capability
`P-03` withholds. Handing it raw OCR would be.

### 6.2 The container, and a reader for it

**The container.** One schema in this repository — call it
`ProjectContentPack` — defining a canonical JSON header
(`schema`, `root`, `record_count`, `directory[]` of `{key, offset, length}`)
followed by opaque record bodies. The framework validates the envelope at
registration and never parses a body. Version it, and let the same format serve
every project.

**The reader.** The pure sandbox has no JSON decoder, so today every project
writes one in Luau under a 2,000,000-tick budget. That is infrastructure
re-derived per game, and it is precisely where a project will get it wrong.

> **Answered 2026-08-12, and neither shape below was built.** `artifact.read`
> returns the decoded frozen value, so the header and the record bodies of a
> pack arrive as tables and there is nothing for a prelude to decode and
> nothing for a slice to avoid materialising. What survives of this section is
> the *container's* value: a directory of `(key, offset, length)` bought
> per-candidate cost, and under a value boundary the whole pack is built on the
> first read of that root instead. If a pack ever approaches the quota that
> trade returns, and `artifact.slice` is the answer then, not now.

[What the pure plugin environment owes a project](2026-08-12-plugin-pure-helpers.md)
was written the same day and asks this question from the other side — what the
23-name environment should expose at all. **That document owns the ruling; this
section states only what the container needs from it,** and the two must not
land two readers. Two shapes, both precedented:

- *A frozen prelude.* `pure-data-program.cpp:78-142` already compiles a bridge
  module into every pure VM. A second frozen module exposing
  `content.open(bytes) -> {keys(), record(key)}` plus a bounded JSON decode
  costs the framework one Luau source and no new host surface. Recommended.
- *Extending the frozen reader* with `artifact.slice(name, offset, length)` so
  the plugin never materialises the whole blob. Cheaper on quota, but it widens
  the one frozen host callback and needs its own ruling. Measured relevance: the
  event pack is 1.551 MiB against a 16 MiB quota, charged once per VM, so this
  is not needed today and is the answer if a pack ever approaches the quota.

### 6.3 One statement of the ceilings, with the reason beside them

`k_maximumArtifactBytes` and `k_maximumTotalArtifactBytes` are duplicated
literals in two translation units with no `static_assert` holding them together
and no recorded rationale. Whatever else is decided: state them once, state that
they serve `k_memoryQuotaBytes`, and make the two spellings mechanically the
same value.

### 6.4 A test that exercises the path, and can fail

No plugin has ever called `artifact.read`. A conformance case that registers a
several-hundred-KiB pack, has the plugin seek one record by key and return a
value derived from it, proves a path that today is proven nowhere. Per the
falsification discipline: it must be shown red when the record is removed or its
offset corrupted, or it guards nothing.

> **Partly discharged 2026-08-12, and this item is still owed.** The value
> boundary landed with cases that read an artifact and return what came back,
> each shown red on its own property — decoded, frozen, and one value per VM —
> plus a registration refusal measured on a document that parses and cannot be
> built inside the quota (`tests/script/test-pure-data-boundary.cpp`,
> `tests/operator/test-project-plugin-contract.cpp`). What is still owed is the
> half this section names: a **conformance** case, on a real several-hundred-KiB
> pack, driven from a consuming project rather than a fixture.

## 7. What `uf-chaos`'s build must emit differently

1. **Stop treating `event-content-graph.json` as a deployable.** It is the audit
   artifact. Keep it in the producer's store and pin its digest — the precedent
   is already ruled in `2026-08-11-consumer-attestation.md` §2, which keeps
   per-entity ledgers over the 1,942 source entries out of the registration and
   carries `report_hash` instead.
2. **Emit one `ProjectContentPack` per surface.** For the event surface, today,
   the build would emit: 884 records, 1,626,348 bytes of bodies, a 27,892-byte
   directory, and a 102,220-byte first-pass index.
3. **Drop from the deployed pack, keeping them in the audit artifact:** the
   duplicate top-level `links` array (17.33 MiB); `raw_variants` where it equals
   `raw_fields` (10,405 of 10,842 entities); the 7× `source_ref` amplification
   (87,036 occurrences of 12,440 distinct); and the `en` records that carry no
   English (5,504 entries, 0 `Resolved`, 5.1 MiB across `localized_text` and
   `.en` links).
4. **Keep every honesty field in the projection.** `interpretation_status` per
   entity and `resolution` per link must survive into the deployed record, or
   the pack silently upgrades `Conflict` to `Known` and breaks `D-04`, `D-07`
   and §4.5. The 1.551 MiB measured above already carries them.
5. **Resolve or declare the 334.** 334 of 884 encounters are `Conflict` with
   zero resolved options while declaring 1–3; 1,022 `has_option` links are
   `VariantConflict`; the build report names five conflicting table paths. Until
   §4.4's projection is decided for those paths, recognition addresses 550
   encounters (953,988 B) and the pack must say so per record rather than by
   omitting them.
6. **State the candidate key, and emit the index for it.** Name is not a key
   (371 distinct names, 198 shared, one shared by 14). The graph carries no
   map/pool/floor entity — `link_encounter_pack_id` is an `opaque_relation` on
   all 884 encounters — so either the compiler types the pack and set tables into
   entities, or §6.2's "query candidates by run map + encounter pools" must be
   restated as narrowing by (pack, option count) where the pack comes from
   `ProjectState`. Measured, that key gives 126 buckets with a 3,892-byte median
   and a 158,077-byte worst case.
7. **Deploy a pack that exists.** Two content packs sit on disk
   (`a940dc11…` and `c0c97ba2…`); both contain the identical graph
   `sha256:701a5924…` built from the same
   `source_pack_content_identity sha256:24952adb…`, differing only in pack hash.
   The two blobs the deployments actually pin correspond to neither.

## 8. What could not be measured

Stated plainly, because each of these is a number a later reader may want to
treat as established and must not.

- **Luau decode cost is arithmetic, not a run.** §2.4's figures are computed
  from `VM/src/lobject.h` object sizes and Luau's interning behaviour, not
  observed from a VM. No plugin in either tree decodes JSON, and I did not build
  — two other agents hold this tree, and
  `docs/pitfalls/concurrent-agent-builds.md` is why. The figures are the right
  order and are not measurements.
- **The interrupt-tick cost of a Luau JSON decode is unknown, and it is the one
  number that decides whether a whole-corpus decode is even a fallback.** The
  budget is 2,000,000 interrupt invocations; Luau fires the interrupt at loop
  back-edges and calls. A byte-at-a-time scanner over 1.551 MiB is the same
  order as the budget; a `string.find`-driven decoder is far cheaper because the
  scan runs in C. Which side of 2,000,000 each lands on has to be measured
  before §6.2's prelude is written.
- **The 2-second wall ceiling under the write transaction was not exercised.**
  Nothing in either tree has run a `derive` that touches content.
- **Why 4 MiB and 16 MiB is not recorded anywhere.** I searched both trees; the
  handoff records the values, no document records a derivation. §3 argues from
  the quota because the quota is the only stated reason, not because a document
  says those two numbers were chosen from it.
- **The recognition field list comes from §6.2, not from a capture.** I did not
  analyse a screenshot of the event surface, so "what is actually on screen" is
  the design document's claim, not an observation of mine. In particular §6.2
  names an *icon* and the graph has no icon field on any of the five types.
- **The resolver's full capability was read, its limits were not tested.** I
  read `observe.luau`, `evidence.luau` and `resolution.luau` and can say what
  they do serialize; I did not run them, so "the reading cannot reach the
  plugin" is read off the code path and the `additionalProperties: false`
  schema, not observed failing.
- **The spec bundle is mid-revision.** `docs/architecture/spec-bundle.manifest.json`
  declares bundle 1.10 with `uf-chaos-project-layer-design.md` at 48,824 bytes;
  the file on disk is 29,857 bytes. Another agent owns that directory. Every §4,
  §6 and §7 quotation above is from the on-disk file as of this reading, and may
  not match the bundle the manifest describes.

## 9. Open questions for the owner

1. **Does the `ProjectContentPack` container belong upstream?** §5 says yes on
   the tie-break. If it does not, A is the fallback and `uf-chaos` should be
   told to expect 64 roots to be a real ceiling.
2. **Prelude or `artifact.slice`?** §6.2 recommends the prelude on the measured
   ground that 1.551 MiB against a 16 MiB quota does not need the slice. The
   answer changes if a second surface's pack is much larger.
3. **Does a `ProjectPlugin` receive normalised readings?** §6.1 argues `P-03`
   does not forbid it. This is the ruling the block actually turns on, and it is
   larger than the content question that prompted this document.
4. **Who owns the `Conflict` 334 — the compiler or the design?** §4.4 says a
   table with no provable unique semantic view fails or degrades to Opaque. What
   is on disk degrades to `Conflict` and keeps going. That is defensible; it is
   not currently stated as the decision.
