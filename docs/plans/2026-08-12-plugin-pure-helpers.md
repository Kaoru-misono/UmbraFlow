# What the pure plugin environment owes a project, and what its schemas do not

Status: **largely landed, and one of its recommendations was refuted by trying to
build it.** The decoded frozen value and the `plugin_environment_hash` pin are in
the tree. `canon.encode`, prescribed in §5, was **not** built: an external review
ruled that the host mints `choice_instance_id` from the resolved binding, the
surface generation and the slot, which removes the only argued caller — and a
search found no other, in either tree, after the six plugins were migrated. So
`canon` carries two frozen empty tables and no functions. Read §5 as the record
of an argument that a later ruling made unnecessary, not as work owed.

**Extended 2026-08-12 to the other half of the boundary.** §5 moved the *call*
input and output to a value and left `artifact.read` returning bytes. That
half-measure was the same defect one step out: a project ships no C++ and the
whitelist has no decoder, so an artifact carrying JSON was a capability no
plugin could express. `artifact.read` now answers with the decoded, frozen
value; every registered artifact is parsed and admitted at registration, where
the artifact's own byte ceilings apply and the call-input `ValueBudget`
deliberately does not; and one artifact yields one value per VM however often it
is read. §4's pin was extended with it — the preimage now carries a versioned
contract descriptor per published function, because a preimage over member
*names* left this exact upgrade invisible to `plugin_environment_hash` and to
every `session_manifest_hash` and `decision_basis_hash` beneath it.

Two questions are answered together because they are the same boundary from
opposite sides: §1–§7 decide what a plugin is handed, and §8 decides what is
checked at the moment it is handed over.

Date: 2026-08-12

Scope: `umbraflow-cpp` at `23b25f7` on `design/annotation-system-v2`, plus
read-only measurement of `E:\umbraflow-projects\uf-chaos` (spec bundle v1.10,
`spec-bundle.manifest.json`). Both trees were read only. No git command that
writes was run in either. Two agents were editing this framework and one was
editing uf-chaos's `docs/architecture/` while this was measured; every quoted
fact carries its file and line so a reader can re-check it against a later tree.

Related: [a project is a directory of data](2026-08-11-project-as-data.md)
specifies the directory and the loader rules R1–R9 this document leans on.
[Checks that cannot fail](../pitfalls/checks-that-cannot-fail.md) is the record
§8.5 is measured against. Nothing here changes either.

## 0. The two questions

**Q1.** The plugin VM publishes 23 globals and one frozen `artifact` table. There
is no hash primitive, no JSON parser and no canonical encoder. Every project
must therefore write its own JSON reader and its own structural fingerprint —
the same defect as a consumer writing a JSON Schema validator, one level up.
What should the pure environment expose, without widening the capability surface
the design exists to keep narrow?

**Q2.** Are the schemas worth their complexity? Split three ways: RFC 8785
canonical-byte checking (§8.1), framework-owned document formats (§8.2), and
project payload schemas (§8.3) — the last being the one in doubt.

The answers interact in one direction only, and it is worth stating up front
because it makes the decisions separable: **the recommendation in §5 survives
§8's ruling whichever way it goes.** It rests on the host having parsed the
input document, and the host must parse it to check canonical form (§8.1,
which stays) even if every project payload schema is deleted.

**A1, in short.** Give the plugin a decoded frozen value instead of a byte
string, and one frozen table `canon` holding `encode`, `null` and `emptyObject`.
The host already parsed the document — twice — and throws the result away (§1).
This is the only option that *narrows* the capability surface while closing the
gap: no new global, no framework Luau in the VM, one JSON authority instead of
two, and the plugin can no longer emit non-canonical bytes because it no longer
emits bytes. No hash primitive; the fingerprint becomes the canonical bytes
themselves, which are injective where a digest is one-way (§3).

**A2, in short.** Keep all three categories and delete about 60% of category 3's
bytes without deleting one check. The complaint is right about the volume and
wrong about the cause: **3,584 of the 5,996 lines are one 326-line block written
eleven times**, and the framework causes it by compiling every project schema
with an empty referenced-document set while granting itself cross-document
`$ref` in the same function (§8.3). Publishing `umbraflow-fact-v1` and opening
that set — already owed by `uf-chaos-project-layer-design.md` §12 item 7 —
removes the duplication and nothing else. What the schemas do carry that the
plugin's code does not state is a set of cross-member conditionals; those stay,
and they need falsifiers, because nothing has ever asked one of them to refuse
anything (§8.6).

## 1. What a plugin is handed today, measured

`modules/script/source/script/ffi/pure-data-program.cpp:63-76` publishes exactly
these 23 names into the plugin environment:

```text
assert error getmetatable ipairs next pairs pcall rawequal rawget rawlen rawset
select tonumber tostring type typeof unpack xpcall bit32 math string table utf8
```

`setmetatable` is not among them, though the comment at
`tests/operator/test-project-plugin-contract.cpp:351-353` asserts it is
published "on purpose (k_pureGlobals)". That comment is wrong about the tree it
sits in; only `getmetatable` is published. Nothing depends on the error, and it
is noted here rather than fixed because this document writes no code.

`pushArtifactReader` (`:208-215`) adds one frozen table, `artifact`, with a
single `read(name)` closure. `pushPureEnvironment` (`:217-241`) attaches no
metatable, so there is no `__index` path to the main globals, the registry or a
host table. `runFresh` (`:410-457`) nils `math.random`, `math.randomseed` and
`string.dump` before `luaL_sandbox`.

The data boundary is bytes. `PureDataProgram::invoke` takes a
`std::string_view` and returns a `Result<std::string>`
(`modules/script/source/script/pure-data-program.hpp:46-48`), and the bridge's
`canonical.accept` (`pure-data-program.cpp:88-99`) enforces only that the value
is a string of 1..1 MiB.

**The promise this breaks.** `umbraflow-game-automation-final-design.md:433`
says a plugin module's "唯一可捕获能力是 frozen canonical helpers 与 pinned
immutable artifact readers". There are no canonical helpers. Half of that
sentence describes nothing.

**What the projects do instead.** Every plugin in either tree parses its input
with a Lua pattern and writes its output by string concatenation:

- `examples/arcana-expedition/plugin/expedition.luau:32`,
  `examples/umbraflow/plugin/alpha.luau`, and their two siblings;
- `E:\umbraflow-projects\uf-chaos\plugin\dream.luau:59` —
  `string.match(input, '"tool_name":"([^"]*)"')` — and `:68`, which reads
  `"resolution"` the same way.

`uf-chaos-project-layer-design.md` §13 forbids exactly this: "用 DB option ID、
屏幕序号或展示文本代替当前 observed choice instance". A pattern match over raw
JSON is a display-text read with extra steps. `dream.luau` does not derive a
`choice_instance_id` at all — line 6 carries one as a hardcoded constant.

**What the host already did with the same bytes.** By the time the plugin sees
its input, the host has parsed it twice:

- `ProjectSchemaOwner::canonicalize` → `deployment::canonicalJsonValidator` →
  `json::requireExactCanonical`, which parses and re-serializes
  (`modules/json/source/json/value.hpp:116-121`);
- `ProjectDeployment::documentValidator`
  (`modules/deployment/source/deployment/project-deployment.cpp:1792-1811`),
  which calls `parseDocument` (`:759-769`), validates the resulting
  `json::Value`, **and discards it**.

`ProjectPluginHandle::invoke`
(`modules/operator/source/operator/project-plugin.cpp:324-336`) then hands the
plugin the byte string, and the plugin re-derives structure from it with
`string.match`. The output path repeats the pair. So one `derive` call parses
the same document twice on the way in, twice on the way out, and once badly in
between. The parse a plugin needs has already been paid for and thrown away.

## 2. The options

### 2.1 A — a frozen Luau helper table injected at load

The shape `k_bridgeSource` already has: Luau source compiled by the framework,
loaded into the same fresh VM, exposed as a frozen table
(`canon.decode`, `canon.encode`, `canon.null`).

There is a working encoder to reuse: `modules/task/runtime/jcs.luau`, 341 lines,
RFC 8785, held to `tests/vectors/jcs-vectors.txt` by `tests/task/test-jcs.luau`.
There is no decoder anywhere in Luau in either tree.

Costs.

- **A module-boundary problem.** `jcs.luau` lives in `task`, whose manifest
  declares `public = core domain engine script trace` — `task` is *above*
  `script`. `script` cannot reach it without inverting the dependency, and
  `script` has no `[embed]` block. So A means either copying the file under
  `script` or moving it down and giving `task` a dependency on it.
  `tests/vectors/jcs-vectors.txt:1-6` states that RFC 8785 is already spelled
  three times in this repository and that bytes produced by one spelling are
  hashed and verified by another; a copy makes it four.
- **A decoder must be written.** ~300 lines of new Luau running inside the
  quota, in the language with the weakest tooling in the tree.
- **Two spellings that must agree by test rather than by construction.** The
  plugin's fingerprint bytes would come from the Luau encoder while the host
  canonicalises the same value with the C++ one. A disagreement is not a
  refusal; it is an identity that silently differs from the one the host would
  have computed.
- **`jcs.luau` cannot express an empty JSON object.** Its own header says so
  (`:230-233`): Lua cannot distinguish an empty array from an empty object, and
  it encodes both as `[]`. A canonical encoder for plugin output cannot carry
  that hole.
- **Replay.** The helper's bytes are an input to a deterministic function whose
  output is hashed into the ledger, and no hash covers them. See §4.

Capability surface: no new authority. Pure Luau, quota-bound, no clock, RNG,
handle or I/O. What it widens is the amount of framework-authored code inside
the sandbox and the number of things a reviewer must keep true across versions.

### 2.2 B — native pure functions bound into the VM

The shape `artifact.read` already has: a `lua_pushcclosure` over host state
(`pure-data-program.cpp:208-215`), published in a frozen table with
`lua_setreadonly`. `canon.encode` would be a thin call to
`json::canonicalBytes`; `canon.decode` a call to `json::parse`.

Costs.

- `script` must depend on `json`. `json`'s manifest declares `public = core`
  only, so `script → json` closes no cycle and `python scripts/check_modules.py`
  would accept it.
- Conversion code between `json::Value` and Luau values lands in `ffi/`, the
  most safety-critical file in the module, and every raw operation needs a
  `// SAFETY:` justification.
- Same replay obligation as A (§4).

Capability surface: one native call reachable with plugin-controlled strings,
which is a fuzz surface `artifact.read` does not have — `artifact.read` only
matches a name against a fixed set, while a decoder runs a parser over arbitrary
input. Against that, `json::parse` is already run over adversarial input on the
host path and bounds depth at 64 (`modules/json/source/json/value.cpp:32`).

The decisive property is that there is **one** JSON authority rather than two:
the bytes the plugin sees decoded and the bytes the host validated are the same
function of the same input, by construction and not by test.

### 2.3 C — the host hands the plugin a decoded frozen value

The host already holds `json::Value` for every document the plugin receives
(§1). Push it into the VM as a frozen table instead of pushing the bytes; take
a value back instead of bytes and canonicalise it host-side.

What it removes.

- **The decoder disappears entirely** — not moved, removed. There is nothing to
  write, nothing to hash, nothing to keep in step.
- **Non-canonical plugin output becomes unrepresentable.** Today the plugin
  emits bytes and `schemaOwner.canonicalize` refuses them if they are not exact
  JCS. If the plugin returns a value, `json::canonicalBytes` *mints* the
  canonical form and that refusal path has nothing left to refuse.
- **Two of the four parses per call.** Input is parsed once to validate, and the
  same `Value` is pushed. Output is validated as a `Value` and serialized once.

What it costs.

- **Two sentinels a project author must learn.** A Lua table cannot hold `nil`,
  so JSON null must be a value — `jcs.luau:24-28` already solved this with a
  frozen unique table compared by `rawequal`. And an empty object cannot be told
  from an empty array, so `canon.emptyObject` is needed as a second sentinel.
  Under C the same sentinel rules govern both directions, so the round trip is
  total; under A the encoder's hole (§2.1) has no counterpart to close it.
- **Memory.** A 1 MiB document (`k_maximumDataBytes`,
  `pure-data-program.cpp:54`) currently costs 1 MiB inside a 16 MiB quota
  (`:59`). The same document as Luau tables costs several times that. **Not
  measured.** See §9.
- **`ffi/` conversion code in both directions,** with the same `// SAFETY:`
  burden as B.
- **A byte ceiling that no longer has bytes to measure.** The input bound must
  become a bound on the value — depth and node count — or be applied to the
  canonical bytes the host already holds.

Blast radius, which is smaller than it looks. `ProjectPluginHandle::invoke`
keeps its `CanonicalJson const&` parameter: the Operator assembles envelopes as
bytes and will continue to. Only `PureDataProgram`'s own signature changes, plus
the mint of `CanonicalJson` from a `Value` inside `ProjectSchemaOwner`, which is
already the only type allowed to mint one (`project-plugin.hpp:46-48`).

Capability surface: **narrower than today.** No new global, no native function
reachable from plugin code, no framework Luau in the VM. The whitelist stays at
23 and `artifact` stays the only frozen table.

One objection deserves an answer. "The core only understands opaque project
payloads" (`umbraflow-game-automation-final-design.md:461`) — does decoding the
payload break that? No. The host already parses the whole document including the
payload; it must, to validate the envelope around it. Opacity is about not
*interpreting* members, and pushing a table interprets nothing.

### 2.4 D — status quo

Every project writes its own reader. Measured cost today: four framework fixture
plugins and two deployed uf-chaos plugins, all reading JSON with `string.match`,
in violation of the project design's own §13. The next project writes a seventh.

### 2.5 What each does to the capability surface

| | New globals | New native calls | Framework Luau in the VM | New authority | JSON authorities |
|---|---|---|---|---|---|
| A frozen Luau helpers | 1 frozen table | 0 | +~600 lines | none | 2 (agree by test) |
| B native functions | 1 frozen table | 2 | 0 | none | 1 |
| C value boundary | 0 | 0 | 0 | none | 1 |
| D status quo | 0 | 0 | 0 | none | 1 host + 1 per project, ad hoc |

None of the four adds authority. That is the point worth holding onto: the
argument between them is not about what a plugin is *permitted* to do.

## 3. The fingerprint, and whether sha256 belongs in the sandbox

`uf-chaos-project-layer-design.md` §6.3 rules that `semantic_fingerprint` is a
structural encoding built from `bit32`/`string`/`utf8`, that it may collide, and
that collisions fail closed; §13 lists "往上位 pure-data 插件沙箱里加 hash 原语"
as an explicit rejection.

**The project's own artifacts contradict that ruling, and I could not find a
reading in which all three stand together.** Three measurements:

1. `E:\umbraflow-projects\uf-chaos\modules\project_state\identity.py:195-197`:
   ```python
   body = jcs_dumps({"schema": IDENTITY_SCHEMA, **payload})
   return f"{prefix}:sha256:{hashlib.sha256(body.encode('utf-8')).hexdigest()}"
   ```
   The reference implementation's fingerprint is `sha256` of exact JCS bytes.
2. `schema\dream\project-observation-v1.schema.json:284-287` requires
   `"pattern": "^choice:sha256:[0-9a-f]{64}$"` on `choice_instance_id`, and
   `:26-29` the same for `^evt:sha256:...`. `schema\dream\tool-precondition-v1.schema.json`
   requires `^(battle|camp|card|choice|evt|product|quest|reward|route|unlock):sha256:[0-9a-f]{64}$`
   on every tool's `observed_instance_id`.
3. The plugin cannot produce a sha256 digest. `dream.luau:6` therefore carries
   `choice:sha256:9669088b…` as a literal.

So the deployed schemas mandate an identifier shape the sandbox provably cannot
mint, and the only implementation that mints it is Python that will not ship.
One of the three must move.

### 3.1 What sha256 would actually widen

A pure `sha256(string) -> string` grants no authority: no clock, no RNG, no I/O,
no handle, no non-determinism. By every test this design applies elsewhere it is
the *least* dangerous thing that could be added — strictly less code than an
encoder, and far less reach than `artifact.read`, which already returns
host-owned bytes.

What it widens is not capability but **explainability**. A digest is one-way: an
identifier appears in `canonical_args`, in a `command_fingerprint`, in the
ledger, and nothing on the host side can say what it was computed from. Exact
canonical bytes are the opposite — they *are* the preimage, injective on the
value model, readable in a divergence audit.

That is the real argument, and it is stronger than §6.3's. §6.3 reaches for the
count of globals because the count is measurable; the count is not what matters.

### 3.2 The digest is a length optimisation, not a requirement

Every consumer of an instance id needs equality and nothing else.
`identity.py:275-282` — `still_present(planned_id, fresh_ids)` — is a membership
test. Nothing in either tree needs one-wayness or fixed width. sha256 is being
used as a shortener.

Whether shortening is needed is a measurable question nobody has asked, and the
answer depends on a list §6.3 says must be written before C2 and which does not
yet exist. Two things make the unshortened form plausible:

- §6.3 already requires the fingerprint input set to be exactly minimal.
- The compounding in `identity.py:237-273` — every choice id nests the whole
  event id, which itself nests the whole event fingerprint — is a reference
  implementation choice, not a requirement. Ids need only be unique within a
  run and are always compared against a fresh observation of the same event.

### 3.3 Three ways forward

- **(i) Canonical bytes as the id.** `canon.encode(fingerprint_inputs)`, used
  directly. Collisions become impossible rather than fail-closed, so §6.3's
  collision rule becomes vacuous — a strict improvement over a rule that has to
  hold. Costs: the id is variable-length, the schema patterns must widen, and
  `identity.py` must be rewritten.
- **(ii) `canon.encode` + `canon.digest`.** Keeps the deployed schemas and the
  reference implementation exactly as they are. Costs one pure function in the
  sandbox and requires §13's rejection to be revisited.
- **(iii) The host mints the id.** Rejected: the core would have to reach into
  the project payload it is required to treat as opaque.

**Recommendation: (i), with (ii) held open pending one measurement.** Take (i)
if the fingerprint input list, once written, yields ids under a few hundred
bytes. Take (ii) if it does not — and if so, reopen §13 on the evidence above
rather than working around a rejection whose premise its own schemas contradict.
Either way the plugin needs `canon.encode`; only (ii) needs a digest.

## 4. Replay, and a pin that is already missing

The brief asks whether a helper whose behaviour can change between versions
breaks the hash chain. It does — and **the chain is already broken in exactly
that way, before any helper is added.**

`k_bridgeSource` (`pure-data-program.cpp:78-142`) is a C++ string literal
compiled fresh at every `PureDataProgram::compile`. It runs inside the VM, it
wraps every call, and it decides what a module may export. Nothing hashes it.
`ProjectPluginRegistrar::registerPlugin` derives `plugin_hash` from the project's
bytes alone, and `SessionManifestSpec`
(`modules/operator/source/operator/manifest.hpp:128-138`) has eight members,
none covering the plugin environment:

```text
hostProtocolSchemaHash  runtimeModelSchemaHash  runtimeModelArtifactRootHash
operatorProtocolSchemaHash  projectRegistrationHash  policyArtifactHash
journalEnvelopeSchemaHash  agentProfileHash
```

So a framework upgrade that changed the bridge would change what plugins do
under a session manifest that did not move. That is a live defect, independent
of this decision, and it is the same shape the pitfall record calls a check that
cannot fail: the manifest promises to pin what a session runs and does not pin
part of it.

Two ways to discharge it, and they are not equally expensive.

- **Pin the environment.** A ninth member — `plugin_environment_hash` over the
  bridge source, the whitelist and any helper bytes. This changes the tuple in
  `umbraflow-game-automation-final-design.md` §12, and it changes
  `schema/umbraflow-operator-v1.schema.json:136-158`, which sets
  `"additionalProperties": false`. That schema's own sha256 is
  `operator_protocol_schema_hash`, so adding a member moves every
  `session_manifest_hash` and every `decision_basis_hash` derived from it. It is
  the right fix and it is expensive.
- **Make the contract external.** Under C the plugin observes only the values
  the schema already describes, in a model fixed by RFC 8259 and RFC 8785. Two
  framework versions that both implement RFC 8785 correctly produce the same
  bytes, and `tests/vectors/jcs-vectors.txt` is the artifact that holds them to
  it. A helper written in this repository has no such external anchor and must
  be hashed; a boundary defined by a standard the repository already tests
  against does not.

This is the single strongest argument for C over A and B, and it is why the
recommendation below keeps the native surface to `canon.encode` — one function
whose contract is a published standard — rather than a helper library.

The bridge itself still needs a pin either way. It is framework Luau with no
external anchor, and it will be whichever option is chosen.

## 5. Recommendation

**Take C for the document boundary, and add exactly one native pure function.**

1. `PureDataProgram::invoke` takes and returns a decoded value rather than
   bytes. The plugin receives a frozen table; it returns a value; the host mints
   canonical bytes from it. No decoder is written, in Luau or in C++, because
   the parse already happened.
2. Publish one frozen table, `canon`, with three members and nothing else:
   `encode(value) -> string` (exact RFC 8785, over `json::canonicalBytes`),
   `null`, and `emptyObject`. The two sentinels are values, not functions.
3. No hash primitive, pending §3.2's measurement. If it is needed,
   `canon.digest` joins the same table and §13 is reopened with the evidence in
   §3, not around it.
4. Pin the plugin environment (§4) as its own change, because the defect
   predates this one.

Why, in one sentence each.

- **It is the only option that narrows the capability surface** while closing
  the gap. The whitelist stays at 23; the plugin gains one frozen table of
  which two members are constants.
- **It leaves one JSON authority.** The bytes a plugin's fingerprint is built
  from and the bytes the host canonicalises are the same function of the same
  value, by construction rather than by a test that two spellings agree.
- **Its contract is external.** RFC 8785 plus a vector file the repository
  already ships, rather than framework Luau that must be hashed to be replayable.
- **It makes `:433` true.** "The only capturable capabilities are frozen
  canonical helpers and pinned immutable artifact readers" describes the tree
  for the first time, without the sentence changing.
- **It removes a failure mode instead of adding one.** A plugin can no longer
  emit non-canonical output, because it no longer emits bytes.
- **The interface change is the framework's, once,** rather than a JSON reader
  in every project forever.

What it is not. It is not cheaper to build than A: the conversion code is real,
it lives in `ffi/`, and the memory question in §9 could force a lower document
ceiling. The case for it is that the cost is paid once, in the tree with the
best tooling, and that the two things it must never get wrong — canonical form
and the value model — are the two things this repository already tests hardest.

## 6. What lands where

Framework, all of it:

| Change | Where |
|---|---|
| `script` gains a dependency on `json` | `modules/script/manifest.txt` (`public = core domain json`; `json` declares `public = core`, so no cycle — `scripts/check_modules.py` enforces this) |
| Value boundary on the plugin API | `modules/script/source/script/pure-data-program.hpp:40-48` |
| `json::Value` ↔ Luau conversion, both directions, with the two sentinels | `modules/script/source/script/ffi/pure-data-program.cpp`, under `// SAFETY:` |
| `canon` frozen table | same file, beside `pushArtifactReader:208-215`, which is its precedent |
| Bridge's `canonical.accept` becomes a value check | `pure-data-program.cpp:88-99` |
| Bounds move from bytes to depth and node count | `pure-data-program.cpp:52-61` |
| `CanonicalJson` minted from a `Value` | `modules/operator/source/operator/project-plugin.cpp:324-336`; the mint stays private to `ProjectSchemaOwner` |
| A `json::Value`-taking document validator, so the parse is not repeated | `ProjectDocumentValidator` (`project-plugin.hpp:96-98`) and `project-deployment.cpp:1792-1811`; this makes `operator` depend on `json` |
| `plugin_environment_hash` | `manifest.hpp:128-138`, `schema/umbraflow-operator-v1.schema.json:136-158` — see §7 |
| Four fixture plugins rewritten to return values | `examples/{arcana-expedition,umbraflow}/plugin/*.luau` |
| Every inline fixture source rewritten | `tests/operator/test-project-plugin-contract.cpp` |
| `canon.encode` held to the shared vectors *from inside the plugin VM* | new case over `tests/vectors/jcs-vectors.txt` |
| Correct the stale `setmetatable` comment | `tests/operator/test-project-plugin-contract.cpp:351-353` |

Falsifiers the new tests owe, since a green test here would otherwise guard
nothing: `canon.encode` must go red at the vector assertion when one vector's
expected bytes are perturbed; the sentinel round trip must go red when
`emptyObject` is mapped to an empty array; the environment pin must go red when
one byte of the bridge source changes.

uf-chaos, which owns these and must make them in its own tree:

- Two plugins rewritten to consume tables and return values
  (`plugin\dream.luau`, `plugin\archive.luau`).
- `modules\project_state\identity.py` rewritten under §3.3(i), or kept under
  (ii).
- The three `sha256`-shaped id patterns widened under (i).
- §6.3 and §13 amended (§7).

## 7. What requires changing the frozen bundle

The bundle is at **v1.10**, not v1.9 — `spec-bundle.manifest.json` reads
`"bundle_version": "1.10"`, while
`umbraflow-game-automation-final-design.md:1132` still says "同一 v1.9 spec
bundle". That drift is not mine and is reported rather than touched.

Requires a bundle change:

1. **§12's compatibility tuple**, if the environment is pinned (§4). It gains a
   member, and `schema/umbraflow-operator-v1.schema.json` is
   `additionalProperties: false`, so the schema's own digest moves and with it
   every `session_manifest_hash` and `decision_basis_hash`.
2. **`uf-chaos-project-layer-design.md` §6.3**, whose measured sentence — 23
   globals "外加一个 frozen `artifact.read`" — becomes two frozen tables, and
   whose ruling that the fingerprint is a `bit32`/`string`/`utf8` structural
   encoding with fail-closed collisions is replaced by exact canonical bytes.
3. **§13's rejection of a hash primitive**, under (ii) only. Under (i) it stands
   and gains a better reason.
4. **§6.3's collision rule and §11's C2 clause** that tests it. Under (i) the
   encoding is injective and the rule has nothing to govern.

Does *not* require a bundle change:

- `:433`, which the recommendation makes true as written.
- `ProjectPlugin`'s five signatures and `ProjectRegistration`'s members, which
  are unchanged.
- Anything in §8, unless §8.6 is adopted — see there.

## 8. Are the schemas worth their complexity?

### 8.1 RFC 8785 canonical-byte checking — keep, and it gets cheaper

Not contested. It is structural, it is the only thing that makes a content hash
mean anything, and `json::requireExactCanonical` answers it by parsing and
re-serializing rather than by recognising shapes, so a document nobody
anticipated is judged too (`value.hpp:116-121`).

One narrowing follows from §5 rather than from doubt: once the plugin returns a
value, its output is canonical by construction and the check on that one path
has nothing left to refuse. Every other document keeps it.

### 8.2 Framework-owned document formats — keep

Also not contested, and the record supports it more strongly than argument does.
The one live victim in `docs/pitfalls/checks-that-cannot-fail.md` — a real
consumer's suite green while producing provenance documents the framework's own
schema would reject — was *statable as a defect only because the schema
existed*. `JournalProvenance` in `schema/umbraflow-journal-v1.schema.json` named
four required members and a five-value `kind` enum; both shipped validators were
one byte comparison. Without the schema there is no fact the validators could be
compared against.

Note also that R1–R9 (`2026-08-11-project-as-data.md:833-930`) are almost
entirely rules about *these* documents and about paths, not about §8.3's
schemas. Deleting §8.3 entirely would leave R1, R2, R3, R4, R7, R8 and R9 intact
and touch only part of R5 and R6.

### 8.3 Project payload schemas — the measurement

There are **23**, not 22: 2 project-state, 2 project-observation, 2
tool-precondition, 2 reconcile, 2 effect-payload, 13 journal payload. (The other
6 files under `schema/` are *instances* of framework-owned formats — the tool
catalogs and the two manifest kinds — and belong to §8.2.) Together they are
**5,996 lines**.

**Finding 1 — the complexity is duplication, and the framework causes it.**

Ten of the eleven journal schemas that have a `$defs` block are **byte-identical
from `"$defs"` to end of file**: `sha256` of that region is `7ea9f836…` for all
ten, 326 lines each. The eleventh, `project.baseline_created-v1`, carries the
same four definitions interleaved with two of its own, byte-identical in
content. That is **3,584 of the journal corpus's 4,320 lines — 83% — as one
block written eleven times.** Four of the files (`battle.completed-v1`,
`camp.action_used-v1`, `card.added-v1`, `reward.observed-v1`, 351 lines each)
are byte-identical to one another except for `$id`, `title` and `description`.

**The coordinator's fact is confirmed and it is the cause.** Every project
schema is compiled with an empty referenced-document set:
`project-deployment.cpp:1458-1473` passes `{}` for project state, observation,
tool-precondition and reconcile; `:1497-1519` passes `{}` for every journal and
effect payload schema. The framework's own schemas in the same function get
`withWorld`, `withState` and `commonOnly`. The framework grants itself
cross-document `$ref` and denies it to projects. All 319 `$ref` occurrences
across the 13 journal schemas resolve as `#/$defs/…` — nobody even tried,
because trying would fail to compile.

So the volume the owner objects to is not the cost of checking. It is the cost
of not being allowed to say a thing once.

**Finding 2 — the checks are not ceremony, and they are not self-agreement.**

Of the 23, the constraints that would refuse a plausible wrong value are
cross-member conditionals, and they state exactly the rules the design says are
load-bearing:

- `dream\project-state-v1.schema.json:118-215` — `status: "Known"` requires
  `value`, requires `provenance` with `minItems: 1`, and forbids `candidates`;
  `"Unknown"` requires `reason` and forbids both `value` and `candidates`;
  `"Conflict"` requires `candidates` with `minItems: 2`. That is §5.2's whole
  point ("字段缺失…不得同时表示零、空集合和未知") in a form something checks.
  Seven such rules in that file.
- `dream\project-observation-v1.schema.json:88-166` — an unresolved event aligns
  nothing; `safe_for_choice_selection: true` requires `resolution: "Resolved"`
  and every visible choice aligned; `ObservedOnly` requires zero content
  candidates. `:328-362` — a `Resolved` choice has exactly one content option
  and effect completeness in `{Complete, Partial}`, otherwise `Unknown`.
- `journal\event.resolved-v1.schema.json` — `Resolved` requires exactly one
  content candidate (`minItems: 1, maxItems: 1`), `ObservedOnly` requires zero.
- `journal\domain.corrected-v1` and `domain.diverged-v1` require `approval`.
- The shared block's `Known`/`Unknown`/`Conflict` pairing rules, inherited by
  all eleven.

**The argument that the plugin "agrees with itself" does not reach these.**
`safe_for_choice_selection` is a claim the Operator acts on: it gates whether a
policy may select a choice. A schema that refuses `true` on an `Ambiguous` view
is not checking self-consistency, it is checking a claim before an authority
acts on it. Same for `approval` on a correction.

The tool-precondition schemas are not a plugin boundary at all. They judge
**caller-supplied** `canonical_args` — from an agent, a script, or a human — and
`additionalProperties: false` over closed argument objects is what enforces §8's
"调用者不能提交 expected effects、风险、UI target、坐标或 Receipt". The
conformance suite already drives the refusal:
`umbraflow-conformance.json:11` supplies
`refused_tool_arguments: {"ui_target_id":"battle.card_slot_1"}` and
`modules/conformance/source/conformance/suite-project-authority.cpp:91-98`
requires it to be refused.

**Finding 3 — which ones do not earn their place.** One whole file, clearly:
`journal\run.ended-v1.schema.json` (26 lines) — `type`, `required`,
`additionalProperties: false` over two hardcoded members, plus a generic id
pattern the plugin satisfies by construction and a `minLength: 1`. Its own
description says it deliberately declined to enum-constrain `outcome`. Beyond
that, the five near-duplicates in the `battle/camp/card/reward/run.started`
family carry no event-specific property at all — no card id, no reward payload,
no camp action name — so what each of them checks is entirely the shared block's,
and five files exist to say one thing about five event names.

Three keyword-level instances, worth naming because they are the shape the
complaint is about:

- `dream\reconcile-v1.schema.json:41-42` and its archive twin carry
  `"minProperties": 1` and `"maxProperties": 1` on an object that declares one
  property, `required`s it, and sets `additionalProperties: false`. Those two
  keywords cannot fail.
- Both reconcile files wrap their two definitions in a top-level `oneOf`, and
  both tool-precondition files in a top-level `anyOf`. **Neither is ever
  evaluated.** Tool arguments are judged by the subschema the catalog row names
  (`project-deployment.cpp:1064`, `toolPrecondition.validateDefinition`), and
  reconcile documents by the definitions the reconcile manifest names
  (`:1154`, `:1205`); no document is ever validated against either root, and
  `withWorld` (`:1435-1440`) — the only reference set the framework's envelope
  schemas get — carries the project's state and observation documents but
  neither of these two. The undiscriminated union is dead, which is fortunate:
  it would accept a `chaos.restock` call carrying `chaos.buy`'s arguments, since
  the branches share no discriminator.

  The same measurement makes the converse true and is worth stating, because it
  is what makes Finding 2's strongest rules live: the project *observation*
  root **is** reached, as a `$ref` from the framework's derive, plan and step
  input schemas, so its three top-level conditionals are evaluated on every one
  of those calls.
- `dream\project-state-v1.schema.json` gives `Fact.value` the boolean schema
  `true` — no constraint at all — in the file whose conditionals exist to say
  when `value` must be present. What a fact asserts is the one member nothing
  judges. That is defensible (the core is meant to be opaque to project values)
  and worth being deliberate about rather than incidental.

**Finding 4 — nothing has ever exercised any of this, on either side.** Two
measurements compound.

Nothing falsifies the schemas: `tests/deployment/test-project-deployment.cpp`
exercises refusals against *fixture* schemas, and the conformance suite drives
exactly one negative case against a real uf-chaos project schema — the
tool-precondition one above. Not one of the conditionals in Finding 2 has a
document that violates it.

And nothing exercises the producer either. In `plugin\archive.luau`, `derive`,
`next_step` and `reduce` ignore their input and return one fixed literal each;
`plan` and `reconcile` parse one member with `string.match` and select among a
closed table of fixed literals. `dream.luau` is the same shape. **No plugin in
either tree assembles a field value from live data.** So today's plugins could
not trip these rules even if the rules were wrong.

That second measurement must not be read as "the schemas are ceremony". It
measures the stub, not the schema: `dream.luau` is 77 lines of constants
standing in for a plugin that will eventually compute, and judging a guard by a
placeholder that cannot reach it is the same error as judging a test by code
that never runs. What it does establish is that **both halves of this contract
are unexercised**, which is why Finding 2's rules are claims rather than checks
by this repository's own standard (`checks-that-cannot-fail.md`: the red must
land at the assertion naming the property). They are the most valuable thing in
the 5,996 lines and the least tested.

### 8.4 Replay across versions — the argument is circular, and something survives it

It is circular as posed. `uf-chaos-project-layer-design.md` §3 requires old
Operations and Journal entries to be replayed "按原 registration/session
manifest" — under the *original* registration, whose plugin and whose schema are
both pinned by the original hash. Byte-identical replay then follows from
determinism alone, and the schema adds nothing. Where the registration inputs
change, §3 requires a replayable migration first and fails closed if there is
none, so the later registration never meets the older bytes unexamined.

What survives is narrower and real: a schema is a **second, independent
statement of the same shape, authored as data, pinned into a hash the ledger can
name.** Its worth is the worth of a second opinion — high when the two
statements are made from different angles, near zero when the same author writes
both in the same hour. That is precisely the split in Finding 2 and Finding 3:
`type`/`required`/`additionalProperties` over members the plugin hardcodes is
the same statement twice; "Known implies value present" is a different statement
that the code does not make anywhere.

### 8.5 What would actually have caught something

Checked against `docs/pitfalls/checks-that-cannot-fail.md`, which is the record
of this question. The hypothesis mostly holds, with one correction that matters.

No defect in that record was found by a project payload schema refusing a
document. The provenance defect (§8.2) was found by reading code; the journal
DDL drift by comparing a schema against a database; the effect-payload digest
gap (`df5a73d`) by flipping a byte and finding no hash moved.

The correction: the two recorded schema failures are both the form "**a schema
file nothing compares against**" (`checks-that-cannot-fail.md:581-600`). The
rule that record derives is to name, for every schema, the producer, the
consumer, and the assertion holding them together. Applied to the 23: the
producer is the plugin, the consumer is the Operator, and the assertion is
`ProjectSchemaOwner::validate`, which runs on every call. **Category 3 passes
the test the recorded failures failed.** What it fails is the falsification test
in Finding 4 — different defect, different repair.

So the evidence says: these schemas are not the recorded failure mode, they have
never caught anything, and nothing has ever asked them to.

### 8.6 Ruling

Keep category 3, delete about half its bytes, and delete none of its checks. The
complaint is right about the volume and wrong about the cause.

1. **Publish `umbraflow-fact-v1` and open the referenced-document set.** This is
   already owed — `uf-chaos-project-layer-design.md` §12 item 7 names both
   halves and says explicitly that publishing the fragment without opening the
   set does not make §5.2 true. It removes ~3,584 duplicated lines, roughly 60%
   of the whole corpus, and removes not one check. `Schema::compile` already
   takes `referencedDocuments` and already resolves within a closed set
   (`modules/json/source/json/schema.hpp:61-69`); the framework uses it for its
   own schemas in the same function that denies it to projects. This is the
   single highest-value change in this document and it is a two-line change at
   `project-deployment.cpp:1458-1519` plus one published file.
2. **Delete `run.ended-v1`,** and let the journal manifest carry no payload
   schema for that event type — provided the framework can express "this event
   type has no payload schema", which it currently may not. Marked unverified.
3. **Delete the four dead keywords in Finding 3:** the two `minProperties`/
   `maxProperties` pairs, and the top-level `oneOf`/`anyOf` unions that
   `validateDefinition` means nothing ever evaluates. A union with no
   discriminator that nothing reaches is worse than absent — it reads as a check.
4. **Collapse the five-file family** once (1) lands: with a shared fragment,
   `battle.completed`, `camp.action_used`, `card.added`, `reward.observed` and
   `run.started` differ only in `$id` and `required`, and are a few lines each.
5. **Give the conditionals falsifiers.** One document per conditional, violating
   exactly one rule, driven through the conformance suite the way
   `refused_tool_arguments` already is and the way `contract-agent-a04` drives
   six for the journal envelope. Until that exists, Finding 2's rules are
   claims.
6. **Do not delete the cross-member conditionals**, and do not accept a project
   schema that has none: a project schema consisting only of `type`, `required`
   and `additionalProperties: false` is the same statement as the plugin's own
   code and should be declined at review.

The order matters. (1) is worth doing on its own and immediately; it is where
the complexity actually is, it removes no check, and it is already owed. (5) is
what turns the rest from claims into checks, and it is worth more than (2), (3)
and (4) combined.

Effect on §5: none. The value boundary needs the host to hold a parsed
`json::Value`, and §8.1 keeps the parse that produces it even if every document
in category 3 were deleted.

## 9. What I could not verify

- **Memory cost of C.** How much a 1 MiB canonical document costs as frozen Luau
  tables against the 16 MiB quota (`pure-data-program.cpp:54,59`) is not
  measured, and it is the one number that could force the design to a lower
  document ceiling or a lazier push. It must be measured before implementation,
  not during.
- **Whether §3.3(i) yields a workable id length.** The fingerprint input list
  §6.3 requires does not exist yet, so the unshortened id's size is unknown, and
  with it the choice between (i) and (ii).
- **Whether the framework can express a journal event type with no payload
  schema** (§8.6 item 2). I read the manifest format but did not confirm that
  the loader accepts an entry naming none.
- **What a real plugin would emit.** Every judgement in §8.3 about whether a
  constraint could refuse something is reasoning about a plugin that computes.
  No such plugin exists in either tree (Finding 4), so no measurement of "the
  producer actually tried to emit this and was refused" was possible for any of
  the 23. That is the gap item (5) of §8.6 exists to close.
- **The 23 globals are pinned by no test.** I searched
  `tests/script/` and `tests/operator/`; the whitelist is asserted only
  indirectly, by `test-project-plugin-contract.cpp:333-378` naming individual
  absent globals. uf-chaos §6.3 cites the count as a measured fact, and nothing
  would go red if it changed.
- **Nothing here was built or run.** `python scripts/fix_format.py --check` is
  the only command this work owes, and no C++ was written to compile.
- **Registration in the documentation index.** `docs/INDEX.md` and
  `docs/plans/README.md` should name this file; both were outside my scope and
  are owed by whoever acts on it.
