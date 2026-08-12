# Consumer attestation: what `attest-consumer-dNN` has to be

Status: specification proposal. Nothing here is implemented, and §5 rules that
implementing it changes no schema and no compiled hash in this repository.
Six questions in §10 need a ruling from the repository owner.
Date: 2026-08-11
Scope: `umbraflow-cpp` only. It defines a document format and a recording
location that a consumer repository fills in; no consumer-project writes.
Bundle: v1.13, root `c8e559a1ee6618246778ac465842976b7445fbe10a20a2edaf77ca047ec6e5f0`
(specified against v1.9; brought current 2026-08-12. Nothing this document
proposes moved with the bundle; §9's corrections cite rows that did not change.
Deciding artifact: the hardening rewrite's frozen authority.)

## 1. The gap, verified

Nine requirements, `D-01` through `D-09`, are assigned to the consumer
repository and marked verified by `EXTERNAL attest-consumer-dNN` in
[the migration report](2026-08-09-runtime-migration-report.md). Those nine
strings occur in exactly one file, one table, nine rows, and nowhere else in
this tree — no format, no schema, no CTest registration, no script, no
recording location, no statement of who signs one or what makes it valid. The
survey's claim holds; `rg` over the whole tree returns only
`docs/plans/2026-08-09-runtime-migration-report.md:65-73`. A consumer that
wanted to satisfy `attest-consumer-d05` today has nothing to produce.

Declining to gate the nine upstream is correct. They are consumer facts, a
fixture could fake every one of them, and that is precisely why a fixture must
not be able to. But declining to gate is not declining to specify, and the
specification is upstream's: the consumer cannot invent a document shape this
repository will later agree to bind into `project_registration_hash`.

This document settles the shape, the binding, the recording location, the
refusal, and the ordering. It does not attest anything and cannot.

## 2. What an attestation is

**Ruled: one exact RFC 8785 JCS document, content-addressed by SHA-256 over its
own identity subset, produced under a file capability held open as a live
descriptor, carrying `report_hash` and never the report itself. Nine of them
are inlined into one `ProjectAttestationSet` document, and that document is the
exact bytes of one named entry in `project_artifact_roots`.**

Not a signature, not a recorded CTest run, not a directory. The reasons are all
precedent rather than taste.

**The tree already has this exact shape for the same problem.** `tools/annotate`
solves "a fact the deployment side cannot recompute" three times, the same way
each time. A trusted runner records a `TrustedReplayResult` — one exact JCS
object, loaded through `load_exact_jcs`, whose `result_id` is
`sha256(jcs(identity_document))` over a fixed subset that deliberately excludes
`result_id` itself, so a caller cannot name its own identity. Publication turns
it into a `ReplayAttestation` that keeps `report_hash` and drops `report`. Two
of those plus one project/operation attestation are **inlined** into a
`ReplayGate` — the schema says why: "They are inlined rather than referenced so
that neither can be published while carrying only the other's evidence." An
attestation set is the same object at a different scale, and inlining is what
stops eight-of-nine from looking like nine.

**A signature would be a second trust model.** This repository has one:
content-addressed identity plus a file capability. `_DescriptorAuthority` in
`tools/annotate/store.py` opens the capability file, hashes its bytes, and
re-checks both the descriptor identity (`os.fstat` before, at open, and after)
and the content hash on every read, refusing with "trusted capability identity
changed". `TrustedCapabilityFile` carries `capability_version`, `nonce`,
`principal`, `purpose`. Adding PKI beside that would mean key distribution,
revocation and expiry beside a mechanism that already produces an attributable
principal — the same argument that rejected a deployment-principal co-signature
over the Tool Catalog in [the next block](2026-08-10-next-block.md) §4. Two
trust models are worse than one documented limit.

The consumer's attestation capability is a file of that **shape**, not an
instance of that **type**: `TrustedCapabilityFile.purpose` is an enum of exactly
`human-review`, `replay-runner` and `publication`, and widening it would edit
`schema/umbraflow-annotation-workspace-v2.schema.json` and move a compiled hash,
for the reasons in §5.3. The authoring workspace is upstream's; the attestation
capability is the consumer's, lives in the consumer's tree, and is never opened
by anything here.

**A recorded run would be a check that cannot fail.** The consumer runs its own
CTest; upstream never sees the process. A "recorded run" that upstream stores
is a claim about a run, which is an attestation with extra steps and a name
that overstates. [Checks that cannot fail](../pitfalls/checks-that-cannot-fail.md)
records four instances of exactly that defect found on one day.

**A directory does not fit.** `verifyArtifactClosure` in
`modules/operator/source/operator/project-plugin.cpp` takes each named root as
one byte string, capped at 4 MiB per blob and 16 MiB in total across at most 64
roots. One JCS document is what a root is. That ceiling is also why the
travelling document carries `report_hash` and not `report`: the reports behind
D-02, D-04 and D-07 are per-entity ledgers over 1,942 source entries and will
not fit, and the `ReplayAttestation`/`TrustedReplayResult` split is already the
precedent for keeping the digest and leaving the body in the producer's store.

### 2.1 Document shape

Field names are lower snake case and every object is `additionalProperties:
false`, matching `schema/umbraflow-annotation-workspace-v2.schema.json`. JCS
sorts keys, so the reader's positional order is the sorted order.

```text
ProjectAttestation {
  attestation_id        // sha256(jcs(this document minus attestation_id))
  requirement_id        // "D-01" .. "D-09"
  bundle_root_hash      // the v1.9 spec bundle root the wording is quoted from
  plugin_id
  plugin_hash
  content_pack_hash
  recognition_pack_hash
  source_snapshot_hash
  build_recipe_hash
  passed                // const true
  report_hash           // sha256 over the exact report bytes, kept by the producer
  attestor_principal    // from the capability file
  capability_hash       // sha256 of the capability file bytes
  created_at
}

ProjectAttestationSet {
  set_version           // const 1
  bundle_root_hash
  plugin_id
  attestations[9]       // one per requirement_id, D-01..D-09, inlined, sorted
}
```

`passed` is `const true` rather than a boolean, as it is in
`TrustedReplayResult`, `ReplayAttestation` and both halves of `ReplayGate`. A
requirement that does not hold has no attestation; there is no such thing as a
recorded failure, because a recorded failure is a row that lets a set look
complete while carrying nothing.

`attestation_id` is derived, never supplied. This is the property the third
adversarial round confirmed for the Replay Bundle — `_bundle_document` refuses a
supplied `bundle_id` before anything else — and the reason is the same: an
identity a producer can name is an identity a producer can reuse.

There is no `verified`, no `status`, and no aggregate. The consumer design's §11
is explicit that the four entity-level validation levels "不得合并成上位明确禁止
的 `verified=true`" — must not be merged into the `verified=true` the upstream
spec forbids. An attestation set is nine facts about one byte set, not a grade.

## 3. The nine requirements, and what discharges each

Quoted from the frozen v1.9 bundle,
`E:\github\uf-chaos\docs\architecture\requirements-traceability.md`
§4, rows `D-01`-`D-09`. Chinese is the original; the English is a rendering, not
a second authority. Where the wording is thin, the mechanism it refers to is in
`uf-chaos-project-layer-design.md` §4, and the release gate it belongs to is
that document's §11 (`C0`-`C4`).

### D-01 — the client snapshot is the authority

> 原始客户端快照是该客户端版本内可提取静态事实的权威，不依赖 BWIKI
> 验收：任何部署实体可追溯到 source entry、native key 和 raw hash；构建无 Wiki 输入

*The raw client snapshot is the authority for the extractable static facts of
that client version, and does not depend on BWIKI. Accepted when every deployed
entity traces back to a source entry, a native key and a raw hash, and the build
takes no Wiki input.*

Discharged by two facts, both of which are properties of the build recipe rather
than of the output. First, a **closed input declaration**: the recipe enumerates
every input by content hash, and the report records that the build ran with no
network access and no path outside the declared inputs. Second, **total
traceability**: for every entity in the ContentPack, a non-empty
`source_locators[]` of `{source_entry_id, row_ordinal, native_key}` plus the
`raw_sha256` of the entry it came from. The report is the count of entities with
zero locators, which must be zero, and the digest of the input declaration.

An honest attestation here also records what it does *not* cover. The
requirement scopes the authority to "extractable static facts of that client
version"; §4.2 excludes server config, account state, random outcomes and
undelivered hotfixes. Those are outside the claim, and the report says so rather
than leaving it implied.

### D-02 — coverage and projection are separate ledgers

> SourceSnapshot coverage 与 zh/en BuildProjection 分开
> 验收：1,942 个扫描项全部为 RawCaptured/CaptureFailed；语言排除不从 ledger 消失

*SourceSnapshot coverage is separate from the zh/en BuildProjection. Accepted
when all 1,942 scanned entries are RawCaptured or CaptureFailed, and a language
exclusion never disappears from the ledger.*

Discharged by a two-column census over the SourceEntry ledger:
`capture_status ∈ {RawCaptured, CaptureFailed}` counted, summing to the scanned
entry count, and a second count of entries the BuildProjection excluded by
locale. The second count must be non-zero if any locale was excluded, and every
entry it names must still be present in the first census. The falsifiable form
is the join: `|ledger| == RawCaptured + CaptureFailed`, and
`excluded_by_locale ⊆ ledger`. The report carries both counts, the scanned total,
and `pack_content_identity`.

The number 1,942 is a measurement of one client version, recorded in the v1.9
stable-fact baseline (§2). The attestation records the count it actually
scanned and the `pack_content_identity` it scanned; a set that reports 1,942
against a different pack identity is claiming a coincidence.

### D-03 — `encounters.json` is a rebuildable view

> `encounters.json` 只能是可重建视图
> 验收：删除事件视图后可由 ContentGraph 重建；角色/卡牌/装备/战斗/地图等不依赖它

*`encounters.json` can only be a rebuildable view. Accepted when it can be
rebuilt from the ContentGraph after the event view is deleted, and characters,
cards, equipment, battles and maps do not depend on it.*

Discharged by a positive control, not by absence. The build deletes the view,
rebuilds it from the ContentGraph alone, and records that the rebuilt bytes are
byte-identical to the deleted ones. A run that never produced the view proves
nothing, the same way W0's sanitizer result would have been worthless without
the deliberate heap-buffer-overflow probe that showed the environment could
report one ([the next block](2026-08-10-next-block.md) §3). The second half is a
dependency edge count: the number of ContentGraph entities whose derivation
reads the view, which must be zero. Report: the two digests, the rebuild's input
closure, and the reader count.

### D-04 — parsing is lossless-first

> 解析 lossless-first，未知列/枚举/效果不能丢弃或猜成空值
> 验收：每项最终为 Typed/Opaque/ParseFailed/ExcludedByLocaleProjection；raw/source
> locator 始终可追溯

*Parsing is lossless-first; unknown columns, enums and effects may not be
dropped or guessed into empty values. Accepted when every item ends as Typed,
Opaque, ParseFailed or ExcludedByLocaleProjection, and the raw bytes and source
locator stay traceable.*

Discharged by a four-way partition with no implicit default — the same
discipline as
[the disposition manifest](2026-08-09-runtime-migration-disposition.manifest.json),
whose 101 paths carry 67 `REWRITE` and 34 `DELETE` and "no implicit/default
disposition". The report is the four counts, their sum against the ledger size,
the count of `ParseFailed` entries that nevertheless carry raw bytes and
diagnostics (which must equal the `ParseFailed` count), and the count of typed
scalars whose source column was absent rather than empty (which must be zero —
that is the "guessed into an empty value" the requirement forbids).

### D-05 — the build is deterministic, content-addressed and rollback-capable

> 构建确定、内容寻址、可回滚
> 验收：同 raw/recipe/compiler/schema/locale 两次隔离构建逐字节相同；manifest root
> hash 可验证

*The build is deterministic, content-addressed and can be rolled back. Accepted
when two isolated builds over the same raw inputs, recipe, compiler, schema and
locale set are byte-identical, and the manifest root hash is verifiable.*

"Isolated" needs a definition, and the requirement does not give one. **Proposed:
two builds are isolated when they share no mutable state and differ in every
input the requirement does not pin.** Concretely, the two runs must differ in
absolute build root path, wall-clock start time, process and user identity, and
temporary directory, and must share the declared input closure by hash and
nothing else — no shared cache directory, no shared package store, no incremental
output. §4.2 already names the non-determinism the content hash excludes:
"提取时间、绝对安装路径等非确定性审计 metadata 不参与内容 hash" — extraction time
and absolute install path are audit metadata and stay out of the content hash.
Two builds that agree because they ran in the same directory at the same second
have measured nothing.

Discharged by recording **both** roots. The report carries, for each of the two
runs, the build root path, the start timestamp, the principal, the compiler
identity, and the resulting `content_pack_hash`; plus the shared input closure
digest. The attestation's `content_pack_hash` must equal both. A report naming
one root, or two roots with the same path, is not an isolation record and should
be refused by whoever reads it.

The rollback half is a separate fact and belongs in the same report: the
predecessor `content_pack_hash` this build supersedes, or `null` for the first,
which is the same `predecessor_publication_id` shape the release manifest
already uses.

### D-06 — two deployment languages, and no fabricated English

> 部署语言只含中文和英文，但不能伪造缺失英文
> 验收：locale coverage/fallback/orphan/collision 报告；缺失返回 Missing(locale)

*The deployment carries only Chinese and English, but missing English may not be
fabricated. Accepted on a locale coverage / fallback / orphan / collision report,
with a missing entry returning Missing(locale).*

Discharged by the four-part report §4.6 already names, per locale: coverage
(keys present), fallback (keys served from `zht` under a caller-declared
fallback), orphan (text resources no entity references), and collision, split
into exact collision and comparison collision. Plus one falsifiable negative:
the count of `en` values equal to their `zht` value, which distinguishes a real
English string from a `zht` string copied into the English column. A copied
string is the concrete form "fabricating missing English" takes, and a coverage
percentage alone cannot see it.

### D-07 — the ContentGraph keeps identity and relations

> ContentGraph 保留 stable entity_id、entity_digest、原生 ID 和双向关系
> 验收：dangling ref、重复键、cardinality、跨 snapshot diff 可审计

*The ContentGraph keeps stable entity_id, entity_digest, native IDs and
bidirectional relations. Accepted when dangling refs, duplicate keys,
cardinality and cross-snapshot diff are auditable.*

"Auditable" needs a report shape, and the requirement does not give one.
**Proposed: one row per finding, never a count alone**, because a count cannot
be audited — it can only be compared to another count. The report has four
sections:

| Section | Row shape | Aggregate |
|---|---|---|
| dangling | `{relation_type, source_entity_id, raw_target_key, resolution}` for every link whose `resolution` is `Missing`, `Ambiguous` or `VariantConflict` | count per resolution |
| duplicate keys | `{type, logical_namespace, canonical_primary_key, entity_ids[]}` for every key held by more than one entity | count |
| cardinality | `{relation_type, observed_min, observed_max, declared_min, declared_max}` per relation type, and the entity ids at each observed extreme | violations |
| snapshot diff | `{entity_id, previous_entity_digest, current_entity_digest, change}` against the named predecessor pack, `change ∈ {added, removed, modified}` | counts per change |

Two constraints on the sections rather than on the counts. Every dangling row
must also appear in the affected entity's `interpretation_status`, because §4.5
requires the resolution to propagate: "不能只在 build report 记录后仍把效果视为
Known" — recording it in the build report and still treating the effect as Known
is forbidden. And the diff section names its predecessor `content_pack_hash`
explicitly, or states `null` for a first pack; a diff against an unnamed
predecessor is not auditable.

`entity_digest` stability is the fifth fact and does not have a section of its
own: it is the requirement that a `modified` row exists whenever the digest
moved, which the diff section already carries.

### D-08 — ExpectedEffect and ObservedOutcome stay apart

> ExpectedEffect 与 ObservedOutcome 分离
> 验收：静态预测不能直接写入 ProjectState；prediction mismatch 保留两侧 provenance

*ExpectedEffect and ObservedOutcome are separate. Accepted when a static
prediction cannot be written directly into ProjectState, and a prediction
mismatch keeps the provenance of both sides.*

This is the one of the nine that is not a content-pipeline fact. Its schema row
in the migration report is `OP:ExpectedEffect/ObservedOutcome + CP`, and both
types live in the Operator protocol, so the claim is about how the plugin's
`reduce` and `reconcile` behave — which only exists once the plugin runs under
the Operator. See §7: this one, and only this one, has the exported contract
suite as a precondition.

Discharged by a case list rather than a census: for each mutating tool the
plugin declares, a recorded reconcile in which the ExpectedEffect and the
ObservedOutcome disagree, and the resulting Journal entries showing both
provenances retained and no ProjectState write attributable to the prediction
alone. The report is `{tool, expected_digest, observed_digest,
journal_entry_ids[], project_state_revision_before, ..._after}` per case, plus
the count of tools with no such case, which must be zero.

The `conformance` case "the reducer is handed exactly the Journal prefix that
is appended" in `conformance/source/suite-control-ledger.cpp` is the upstream
half of the same property and is already gated. D-08 is the project half.

### D-09 — the first phase types only what it needs

> 第一阶段只 typed 首个安全闭环所需表，其余诚实保持 Opaque
> 验收：全量 coverage 不被“全量语义建模”阻塞；Opaque 风险不默认为零

*The first phase types only the tables the first safe closed loop needs, and the
rest stay honestly Opaque. Accepted when full coverage is not blocked on "full
semantic modelling", and Opaque risk is not defaulted to zero.*

**`D-09` is `PHASED`, not `PROJECT_CONTRACT`**, and it is the only row in the
block that is. That changes what its attestation says. The other eight attest a
property that either holds or does not; this one attests a **declared scope
boundary** — where the typed slice stops, and that the rest is marked Opaque
rather than silently absent.

Discharged by two lists and one non-zero. The lists: the logical tables typed in
this phase, with the closed loop each is needed for; and the tables left Opaque,
each with its `interpretation_status`. The non-zero: the risk contribution
Opaque entities make to `ExpectedEffect` completeness, recorded as a number that
is not zero for any effect that reads an Opaque entity. "Opaque 风险不默认为零" is
the falsifiable half — a pipeline that marks entities Opaque and then computes
risk as if they were Known has satisfied the labelling and defeated the
requirement.

The first half, "full coverage is not blocked", is discharged by D-02's census
rather than separately: coverage is the 1,942-entry ledger, typing is the slice,
and the requirement is that the first does not wait for the second. An
attestation for D-09 whose D-02 sibling is absent claims a phase boundary over a
coverage figure nobody recorded.

## 4. Who signs, and what stops a consumer signing for itself

**Nothing stops it, and nothing should.** Upstream cannot verify a single one of
these facts. It has no client snapshot, no build recipe, no ContentGraph and no
way to run the consumer's compiler. An attestation is therefore a claim by the
consumer about itself, and the honest containment is the one this repository has
already ruled twice.

> `p03`'s containment is declaration plus attribution, not prevention. The Tool
> Catalog is project-owned, so a project can mark a coordinate tool `Semantic`;
> `plugin_hash` inside `project_registration_hash` makes that attributable rather
> than undetectable.
> — [the next block](2026-08-10-next-block.md) §4, and
> [the reconciliation](../archive/plans/2026-08-10-w2-w7-reconciliation.md) R4

The same ruling was applied a second time to a plugin under-declaring its own
effects (reconciliation §3.11): "a plugin under-declaring its own effects is
contained by attribution, not prevention." **This document follows that ruling
rather than arguing against it.** A consumer attestation is a third instance of
the identical shape — a project describing itself — and inventing a fourth trust
model for it would be the second trust model the `p03` ruling already rejected.

What an attestation **does** prove:

- that a named principal, holding a file capability whose bytes hash to
  `capability_hash`, claimed a specific property at a specific time;
- about a specific byte set — `content_pack_hash`, `recognition_pack_hash`,
  `source_snapshot_hash`, `build_recipe_hash`, `plugin_hash` — none of which the
  attestation can change after the fact, because they are inside its own
  `attestation_id`;
- against a specific wording, because `bundle_root_hash` pins the requirement
  text the claim is answering. A later bundle that rewords `D-05` does not
  silently inherit the old attestation.

What it **does not** prove: that the property holds. Upstream must never say or
imply otherwise, and the `EXTERNAL` marking in the migration report is what says
so today.

What binds it: the set is one entry in `project_artifact_roots`, so its bytes are
inside `project_registration_hash` (§5). That hash pins the session, and the
session pins every Operation, Journal entry and replay under it. A false
attestation is therefore not merely detectable later — the complete set of work
performed while trusting it is enumerable, because it is exactly the set of
sessions carrying that registration hash. That is the strongest containment
available when the verifier has no access to the facts, and it is stronger than
a signature, which would prove the same principal said the same false thing.

**The consumer's own design already says the same in a different vocabulary.**
§11's validation-level mapping forbids using "C1 已通过" to pass off all entities
as `semantics_known`. The attestation is the machine-readable form of that
prohibition: it names its scope, and its scope is inside its identity.

## 5. Where it is recorded, and how it is refused

**Ruled: the set is one named entry in `project_artifact_roots`, spelled
`attestations`. No schema in this repository changes, and no compiled hash
moves.**

`schema/umbraflow-project-registration-v1.schema.json` already carries
`project_artifact_roots` as an array of `{name, root_hash}` with `name` matching
`^[a-z][a-z0-9_-]*(\.[a-z][a-z0-9_-]*)*$`. `attestations` matches. The
registration is `additionalProperties: false`, but the array is open, so the
consumer adds a root and nothing upstream is edited.

That choice buys a real mechanical refusal for free.
`verifyArtifactClosure` in `modules/operator/source/operator/project-plugin.cpp`
already enforces, at plugin registration:

- **exact closure** — every named root must be supplied and no blob may be
  supplied that no root names ("ProjectPlugin received an unregistered artifact
  blob", "ProjectPlugin artifact blob closure is incomplete");
- **byte identity** — each blob is SHA-256'd and compared to its `root_hash`
  ("ProjectPlugin artifact bytes do not match their verified root");
- **ceilings** — 64 roots, 4 MiB per blob, 16 MiB total.

So a registration that names an `attestations` root and cannot produce the exact
bytes fails to register. A registration that produces different bytes fails to
register. And because the root hash is a field of the registration, the whole
set is inside `project_registration_hash`, which `SessionManifestSpec` pins and
`P-06` binds to the session.

### 5.1 The binding runs downward, and the join closes upward

An attestation cannot name `project_registration_hash`: it is inside it. So it
names the components instead — `plugin_hash`, `content_pack_hash`,
`recognition_pack_hash`, `source_snapshot_hash`, `build_recipe_hash` — and the
registration binds over both the attestation set and those same components. The
join is then checkable in both directions without circularity:

```text
registration                     attestation set
  plugin_hash            ────────► plugin_hash
  project_artifact_roots
    content-pack.root_hash ──────► content_pack_hash
    recognition-pack.root_hash ──► recognition_pack_hash
    attestations.root_hash ──┐
                             └──► sha256(set bytes)
```

That join catches the concrete failure this design is for: an attestation copied
from a previous build, or from another project, into a registration whose packs
it does not describe. It is mechanical, it needs no knowledge of the facts, and
it is the only refusal upstream is entitled to.

`content-pack` and `recognition-pack` above are the consumer's own root names,
following its design's `content_pack_hash` / `recognition_pack_hash`; only
`attestations` is proposed here, and Q1 leaves even that open.

### 5.2 What refuses it, and what a failure looks like

Two refusals already exist and need no new code. **Registration** refuses a set
whose bytes do not hash to the root the registration names, and refuses a
registration that names the root and cannot produce it. **The join in §5.1**
refuses a set whose `content_pack_hash` is not the pack the same registration
pins — that one is a comparison somebody has to perform, and the question is who.

**Recommended: the consumer, not this repository.** The `attestations` blob is
handed to the plugin as a `PureDataProgram::Artifact` alongside its other roots,
so the consumer's own plugin holds both sides of the join and can fail at load
rather than serving a session under a set that describes other bytes.

The alternative — a C++ reader here plus
`schema/umbraflow-project-attestation-v1.schema.json` — is tempting and is a
trap. A reader that validates the document's shape and stops would be exactly
the family [checks that cannot fail](../pitfalls/checks-that-cannot-fail.md)
records: a name that promises verification over a claim nothing verifies. Worse,
it would be in-tree, so a fixture could satisfy it, and the nine rows must stay
`EXTERNAL`. See open question Q2 — this is a recommendation, not a settled
ruling, and Q2 splits it: the schema file and the reader are separable, and the
case for shipping the schema is much stronger than the case for the reader.

### 5.3 What the release manifest would have cost

For the record, because the question will be asked again. Putting the
attestation set into the release manifest is wrong on ownership and expensive on
mechanics.

Ownership first: `ReleaseManifest` describes an **annotation-side RuntimeModel
publication** — candidate, generation, replay gate, RuntimeArtifact root. The
attestation set describes a **project content pack**. Putting one in the other
would put a consumer's content claim inside the upstream UI-model handoff, which
is the boundary `U-07` and `A-06` exist to keep apart.

Mechanics second, and this is the joint bump the block has ruled off-limits.
`ReleaseManifest` is defined in
`schema/umbraflow-annotation-workspace-v2.schema.json`. Adding a field there
changes that file's bytes, therefore its SHA-256, therefore:

1. `k_annotationWorkspaceSchemaHash` in
   `modules/operator/source/operator/runtime-installation.hpp` must change in the
   same commit — `check-repository-surface`
   (`tests/test-runtime-surface.py`) recomputes the file's digest and compares
   it to the constant, so the two cannot drift;
2. `parseReleaseManifest` in `runtime-installation.cpp` is a hand-written
   positional JCS reader — it literally consumes
   `{"annotation_workspace_schema_hash":`, then `,"candidate_id":`, and so on,
   and ends with "release manifest has trailing bytes". A new field is a new
   `consume` at its sorted-key position;
3. `tools/annotate/publication.py` must emit it, and the workspace SQLite schema
   root hash `k_workspaceSqliteSchemaHash` moves if any table changes with it;
4. every fixture release manifest updates, and the C++ and Python sides must
   land together or the deployment refuses every release in the tree.

That is a schema change plus two compiled-in hash changes plus a Python/C++ joint
landing, for a field that belongs to a different owner. The
`project_artifact_roots` route costs zero of it.

## 6. `attest-dual-game-p05`, and the gate that must not move

`attest-dual-game-p05` is the sibling string with the same problem: until this
document it occurred in exactly one row of one table — the migration report's
`P-05` — and nothing said what produces it. It is **not** covered by this
document's format, and the reason is structural rather than an omission.

A `D-*` attestation is one consumer describing itself. `P-05` is a fact about
**two independently owned consumers and one suite** — "`uf-chaos` 与第二游戏通过
同一 contract suite 接入", accepted when "核心无游戏名分支；两个插件使用明显不同
opaque payload 仍通过同一 suite". No single consumer can attest it, because
neither owns the other's registration. It needs two documents of the same shape
— one per consumer, each naming its own `plugin_hash` and
`project_registration_hash` and the `contract_suite_hash` it ran — plus one
observation that the two registrations are independently owned, which is a fact
about people and not about bytes.

That last part is why the gate stays where it is. The migration report's closing
paragraph and the next block §5 both record the real dual-game gate as
`EXTERNAL / NOT_RUN`: "this upstream-only worktree does not own two real consumer
registrations and records no fabricated `project_registration_hash`."

**Relation to the `D-*` block: independent, and neither is a prerequisite of the
other.** Concretely:

- A complete nine-of-nine attestation set does **not** move `NOT_RUN`, and must
  not be read as partial progress towards it. G3 Dual Game is about two plugins
  and one suite; the `D-*` block is about one project's content.
- `NOT_RUN` does **not** block `D-01`-`D-07` or `D-09`. The consumer's own §12
  forbids ordering them behind an upstream gate; see §7.
- They are the **two evidence streams that G4 First Mutation needs together**.
  The consumer's §11 C3 states its own precondition explicitly: G3 precedes G4,
  and "本节其余条件全部满足也不构成开放条件" — satisfying every other condition in
  that section still does not constitute permission to open mutation.

So: same destination, separate roads, and neither road is a detour onto the
other. A fixture may move neither.

## 7. What a consumer does first

**The exported conformance suite is a precondition of `D-08` and of nothing else.**

`umbra-flow-conformance --project <directory>` is the one mechanism that already
turns a consumer's claim into something this repository can refuse. A consumer
supplies a project directory, and `deployment::loadConformanceProject` (named
`loadProject` until the 2026-08-12 split) turns it into a
`VerifiedProjectRegistration`, four schema owners, the exact plugin bytes and
artifact blobs the registration pinned, a vocabulary of documents its own
schemas accept, the RuntimeArtifact root its sessions are pinned to, and the one
capture that artifact's model is resolved against — a PNG of the consumer's own
target, judged against the extent the model itself publishes rather than a
number any document restates. The last two are additions of 2026-08-11; before
them the suite substituted a model and a world of its own, and a consumer's UI
vocabulary was never exercised. The discipline outlived the header that used to
state it: the suite invents no project bytes, no authority is defaulted — a
defaulted authority would be an authority nobody granted — and there is nothing
to fall back on. A directory that leaves one out does not run a suite against
nothing; the load refuses it, and the framework's own schemas are what refuse. A
consumer that passes it has demonstrated that its registration, plugin, schemas
and vocabulary are real, that its own RuntimeModel resolves the surface its
plans name on a capture of its own target, and that the Operator accepted all of
it.

That is a strong precondition and it is the wrong one for eight of the nine.

**Why not a precondition of `D-01`-`D-07` and `D-09`.** Those eight are the C0
and C1 gates of the consumer's own release axis, and `uf-chaos-project-layer-
design.md` §12 rules directly on their ordering:

> C0-C1 只消费客户端原始 DB，不消费 Operator contract 或 Runtime v2 的产物，因此不得
> 排在上游门禁之后；本文不得引入上位 §15 没有的依赖。

*C0-C1 consume only the raw client DB, not the products of the Operator contract
or Runtime v2, and therefore must not be ordered after the upstream gate; this
document may not introduce a dependency the upstream §15 does not have.*

Making the suite a precondition of those eight would introduce exactly the
dependency that clause forbids, in a frozen v1.9 document. An attestation about
bytes can be made before those bytes have ever been exercised through the
Operator; that is what running C0-C1 in parallel with Phase 2A means.

**Why it is a precondition of `D-08`.** `ExpectedEffect` and `ObservedOutcome` are
Operator protocol types. The claim is about what the plugin's `reduce` and
`reconcile` do with them, which is C2/C3 on the consumer axis, which §12 orders
after Phase 2A. There is no way to produce the evidence in §3's D-08 entry
without a plugin the Operator has accepted, so the suite is not an added
requirement — it is the only path to the report.

The resulting order for a consumer:

| Step | Produces | Depends on |
|---|---|---|
| 1. Build the ContentPack | `content_pack_hash`, build report | client snapshot only |
| 2. Attest `D-01`-`D-07`, `D-09` | eight of nine | step 1 |
| 3. Implement the ProjectPlugin and registration | `plugin_hash`, `project_registration_hash` | step 1 |
| 4. Run the exported suite against the real plugin | a suite pass | step 3, upstream Phase 2A |
| 5. Attest `D-08` | the ninth | step 4 |
| 6. Register with the complete `attestations` root | a registration the Operator accepts | steps 2 and 5 |

Step 6 is where the set becomes nine and the registration hash covers it. A
registration created between steps 2 and 5 would carry an eight-entry set, which
`ProjectAttestationSet` rejects by requiring nine — the same reason `ReplayGate`
inlines both halves.

**One thing the suite pass is not.** The consumer design §12.5 already rules it:
"同一套上游 contract suite 分别对 fixture plugin 和 `uf-chaos` 真实插件运行；fixture
那次运行不能代替真实插件那次，也不能代替项目 C0-C3." The two in-tree fixture runs,
`conformance-umbraflow` and `conformance-arcana`, substitute for neither the
real-plugin run nor the project's own gates.

## 8. Obligations outside the `D-*` block

The survey's second claim also holds. Two requirements outside the `D-*` block
carry `PROJECT_CONTRACT` alongside `REQUIRED_CORE`, and they are the only two in
the matrix:

| ID | Marking | Requirement | Acceptance |
|---|---|---|---|
| `C-11` | `REQUIRED_CORE` + `PROJECT_CONTRACT` | ReconcileProposal 支持 Continue/Confirmed/Rejected/Ambiguous/Diverged | Continue 原子提交部分事实但保持 lock；Rejected 仅在全部外部效果未发生时合法 |
| `A-04` | `REQUIRED_CORE` + `PROJECT_CONTRACT` | Journal 只记录已确认或可独立证明的领域事实 | proposed/clicked/expected 不提前写成领域历史 |

*C-11: ReconcileProposal supports Continue/Confirmed/Rejected/Ambiguous/Diverged.
Continue atomically commits partial facts while keeping the lock; Rejected is
legal only when no external effect occurred at all.*

*A-04: the Journal records only confirmed or independently provable domain facts.
Proposed, clicked and expected facts are not written into domain history early.*

Both are counted in the 42 `REQUIRED_CORE` requirements and both already own a
`contract-*` gate here, so nothing in this repository's arithmetic changes. What
changes is the consumer's obligation list: it is **eleven** requirements, not
nine — the `D-*` block plus the project halves of these two.

The two halves are not in the same state, and the difference matters:

- **`C-11`'s project half is already covered by something a consumer runs.**
  `contract-control-c11` is a case in the exported suite, so every consumer's
  run executes it. Only this repository's own registration claims it as a
  per-requirement CTest; a consumer runs `umbra-flow-conformance --project
  <directory>` against its own tree, and every case runs in that one process.
  Either way a consumer that passes the suite has exercised its own disposition
  mapping. The format is explicit that the project owns it: the framework's own
  conformance schema requires every vocabulary to carry `continue_input`,
  `confirmed_input`, `rejected_input` and `ambiguous_input`, because the
  project's plugin decides the mapping and the suite must never assume the
  disposition is spelled in the request. No attestation is needed. The suite
  pass is the evidence.
- **`A-04`'s project half is covered by nothing a consumer runs.**
  `contract-agent-a04` is in `tests/operator/test-agent-audit-contract.cpp`, an
  in-tree test, and it is not in either fixture's `CASES` list. The upstream half
  — that the Journal machinery only accepts confirmed facts — is gated. The
  project half — that *this plugin's* reducer does not write proposed, clicked or
  expected facts as domain history — is asserted by nobody. That is a real gap of
  the same kind this document exists to close, and it is not one an attestation
  is the natural answer to: unlike a build determinism claim, it is a behaviour
  the exported suite could exercise, given a project vocabulary that supplies an
  unconfirmed fact. See open question Q4.

## 9. Corrections to the record, 2026-08-11

Applied to [the next block](2026-08-10-next-block.md) §1 and to
[the migration report](2026-08-09-runtime-migration-report.md) §"Requirement
ownership and test map" in the same change as this document. Nothing landed is
rewritten to match; the notes are dated and name the deciding artifact.

**`D-09` is `PHASED`, not `PROJECT_CONTRACT`.** The next block §1 said
"`D-01`-`D-09` are `PROJECT_CONTRACT`". The deciding artifact is the
requirements matrix, `requirements-traceability.md` §4 row `D-09`, under bundle
root `c8e559a1ee6618246778ac465842976b7445fbe10a20a2edaf77ca047ec6e5f0`. The
read was made on 2026-08-11 against v1.9, root `c4760bb5…bfb6a966`; §4 is
unchanged at v1.13; the requirements document's later edits only follow two
evidence paths into `legacy/` and record that move. Read
directly: eight rows carry `PROJECT_CONTRACT` and `D-09` carries `PHASED`, the
only row in the matrix that does. §1 of the matrix defines `PHASED` as
"架构已确定，但按垂直切片逐步开放" — the architecture is settled but opens by
vertical slice.

The correction is not cosmetic. §3's `D-09` entry above turns on it: a `PHASED`
requirement attests a declared scope boundary, and a `PROJECT_CONTRACT`
requirement attests a property. Reading `D-09` as `PROJECT_CONTRACT` would ask
the consumer to attest that the typing is complete, which is the opposite of what
the requirement says.

**The `PROJECT_CONTRACT` set is not the `D-*` block.** The same sentence implied
it was. `C-11` and `A-04` carry it too (§8), so the consumer's obligation list is
eleven requirements. The 42 / 51 arithmetic in the next block is unaffected and
correct: 51 rows total, 42 `REQUIRED_CORE` including `C-11` and `A-04`, 8
`PROJECT_CONTRACT`-only, 1 `PHASED`.

**The migration report's nine `EXTERNAL` rows now have a specification.** They
carried a verification ID and no definition of what produces it. The rows are
unchanged; a dated note points here.

## 10. Open questions

Six, each with a recommendation. None is decided by this document.

**Q1 — is `attestations` the right root name, and does one root or nine belong in
`project_artifact_roots`?** One root holding an inlined set follows `ReplayGate`
and makes eight-of-nine impossible; nine roots would let a registration name
eight and look complete. *Recommend one, named `attestations`.* The cost is that
adding a tenth requirement later rewrites the whole blob and moves the
registration hash — which is correct behaviour, not a defect.

**Q2 — does this repository ship a JSON schema for the attestation set?** §5.2
recommends no C++ reader. But a *schema file* with no in-tree gate is a different
question, and it is the half that is genuinely upstream's to own: the consumer
cannot invent a shape upstream will later agree to. *Recommend shipping
`schema/umbraflow-project-attestation-v1.schema.json` with no C++ reader and no
CTest gate, listed in the migration report's schema table as `PA`.* The risk is
that a schema in `schema/` looks gated when it is not; the mitigation is one
sentence in the schema's own `description`, and `check-repository-surface`
already refuses game symbols in generic schemas. **This is the question most
likely to be answered differently by the owner**, because it trades a real
documentation gain against the "checks that cannot fail" family.

**Q3 — is "isolated" as §3's `D-05` entry defines it strong enough?** The
proposed definition requires differing build root, clock, principal and temp
directory, and a shared input closure by hash. It does not require a different
machine or a different OS kernel, which would catch a compiler that embeds host
state. *Recommend the stated definition for the first attestation and a separate
ruling on cross-host reproduction before G4*, because requiring two hosts now
blocks C1 on infrastructure the consumer does not have.

**Q4 — how is `A-04`'s project half gated?** Three options: (a) an attestation,
which is weak because it is a behaviour a suite could exercise; (b) a new case
in the exported suite, requiring the vocabulary document to gain an
unconfirmed-fact member and every project directory to supply one; (c) leave it,
and record it as a known gap. *Recommend (b).* It is the same shape as `C-11`'s
vocabulary entries, it turns a claim into a run, and the cost is one member in a
document the framework's own conformance schema defines. It is not this
document's to execute — it changes that schema and the loader that applies it,
and this document owns `docs/` only.

**Q5 — does the attestation set need a `predecessor_set_id`?** The release
manifest carries `predecessor_publication_id`, and §3's `D-05` entry already
records a predecessor `content_pack_hash` inside its report. A set-level
predecessor would make the chain of attestation sets walkable independently.
*Recommend no*, on the grounds that the registration hash chain already orders
them and a second ordering is a second thing to keep consistent.

**Q6 — what does a consumer do when an attested property later turns out
false?** Nothing here says. The registration is immutable and the sessions under
it are permanent. The available shapes are a superseding registration (which is
what §3 of the consumer design already requires for any registration input
change, with a replayable migration) versus a revocation record. *Recommend the
superseding registration and no revocation mechanism*: a revocation record is a
new mutable authority, and the existing rule — new registration, new session,
old Operations replay under the old manifest — already produces the right
outcome. It should nevertheless be written down somewhere, because the question
will arrive with the first wrong attestation rather than before it.
