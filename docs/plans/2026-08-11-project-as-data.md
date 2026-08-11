# A project is a directory of data, not a C++ library

Status: specification, ruled. All ten questions are answered in §7.0, and §2, §4
and §5 below are written as those rulings left them. Step 1 of §5 has landed as
`modules/json` (`a0ae304`); nothing else here is implemented.
Date: 2026-08-11
Scope: `umbraflow-cpp`, plus a statement of what the correction costs
`E:\umbraflow-projects\uf-chaos`. Both trees were read only; no file outside
this one was written and no git command that writes was run in either.
Framework read at `6bfe1d6` and reconciled against `319bdb1`; consumer read at
`95d9668` with a dirty tree.

Related: [consumer onboarding](2026-08-11-consumer-onboarding.md) measured what
a consuming repository writes today and proposed moving 626 of 880 lines
upstream. This supersedes its shape rather than its measurements: it answers
"which C++ does a consumer stop writing" with "all of it", which closes its Q1,
Q2, Q4 and Q5 by making them moot and leaves its Q3 (the JCS canonicaliser)
standing as this document's §7 Q1. [Consumer
attestation](2026-08-11-consumer-attestation.md) specifies what a consumer
*claims*; nothing here changes it.

## 0. The decision, and the sentence it invalidates

The repository owner ruled on 2026-08-11:

> uf-chaos depends only on umbraflow's compiled binary, and runs as a plugin
> inside umbraflow.

A consuming project is therefore a directory of data — two root manifests,
schemas, a Luau plugin, a RuntimeArtifact, a probe frame. It has no compiler, no
CMake and no C++ of its own. Everything below follows from that and from nothing
else.

The sentence it invalidates is `cmake/conformance-suite.cmake:6-9`:

> the suite is parameterized by a project rather than by a fixture: the consumer
> writes one translation unit defining
> `uf::operator_runtime::conformance::provideProject`, and calls the function
> below.

`provideProject` (`conformance/include/conformance/provider.hpp:219-220`) returns
a `ProvidedProject` (`:166-205`) whose members are C++ values, five of which have
private constructors. A directory of data cannot produce them, so either the
decision is wrong or that function is. The decision is not wrong, so this
document is about what replaces the function.

The cost of the present shape, measured rather than asserted.
`E:\umbraflow-projects\uf-chaos\contract\` is 3,150 lines of C++ across five
files (`provider.cpp` 1,462, `json-schema.cpp` 828, `json-value.cpp` 719,
`json-value.hpp` 88, `json-schema.hpp` 53), plus 269 lines of CMake and 401 of
README explaining them. Two of those files — a JSON parser and a JSON Schema
2020-12 evaluator — are the framework's work done in a consumer's tree, and
every future project would write them again. The root `CMakeLists.txt` (60
lines) exists only to `add_subdirectory` the framework so that
`uf_add_conformance_suite` is defined.

### 0.1 What is already built, and what is actually missing

It would be easy to read the rest of this document as proposing a plugin
mechanism. It is not. **The mechanism is finished; the loader is the only piece
that has never been written.**

`script::PureDataProgram` (`modules/script/source/script/pure-data-program.hpp:19-49`)
already has exactly the shape the decision needs:
`compile(moduleId, source, entryPoints, artifacts)` and
`invoke(entryPoint, immutableInput) -> Result<std::string>` — one immutable
string in, one out, a fresh quota-bound VM per call. Its header comment
(`:13-18`) closes with "No host installer or native capability seam is part of
this API." The purity is enforced rather than asserted: a 23-name global
allowlist, a 16 MB memory quota, a two-second runtime cap, an interrupt budget
and byte caps on source, bytecode and artifacts, all at
`modules/script/source/script/ffi/pure-data-program.cpp:52-76`. The Operator
already compiles a registration's plugin through it
(`modules/operator/source/operator/project-plugin.cpp:391`).

`ProjectPluginRegistrar::registerPlugin(registration, exactPluginBytes,
artifactBlobs, schemaOwner)` (`project-plugin.hpp:232-235`) already takes
**bytes**. It has never taken anything else.

The call-site census is the whole argument. Across both trees, `registerPlugin`
is called from its own definition, 27 times in
`tests/operator/test-project-plugin-contract.cpp`, six times across
`conformance/` (`suite-project-authority.cpp` ×4, `suite-support.cpp` ×1,
`exemplars/umbraflow/project-fixture.hpp` ×1) — and **zero times in `entry/` and
zero times in `modules/cli`**, which since `319bdb1` are the whole of the
command surface. No shipped binary has ever registered a plugin. The same holds one level up:
every call to `ProjectRegistration::verifyExact` or to any of the four owners'
`create` is in a test fixture or a conformance provider, which is what
`docs/plans/2026-08-10-w2-w7-reconciliation.md`'s predecessor already recorded as
"no production deployment exists yet"
(`docs/plans/2026-08-10-w2-effective-plan.md:1185-1189`).

So the deliverable of this whole correction is one thing: **the code that turns a
project directory into `registerPlugin`'s four arguments, and into the five
authorities beside them.** Everything else it needs already exists and is
already tested.

One thing this document must answer rather than ignore, because the consumer
already wrote the objection down. `contract/CMakeLists.txt:13-17`:

> Every authority this project grants is pinned to the exact bytes of a schema,
> and the provider has to hold those bytes to apply them. They are embedded at
> configure time rather than read at run time: a suite that opened files would
> pass or fail on where it was started from, and the bytes a validator applied
> would no longer provably be the bytes the registration hashed.

The second half is false and the first half is answerable. It is false because
identity is established by hash, not by residence:
`ProjectSchemaOwner::create` hashes the bytes it is handed and compares them
against the registration's pinned digests
(`modules/operator/source/operator/project-plugin.hpp:138-142`, and the same for
the other three owners), so bytes that arrive from a file and hash correctly are
*provably* the bytes the registration hashed, exactly as much as bytes that
arrived from a `constexpr`. Embedding fixes *location*, not identity — and
location is answered by taking the project directory as an explicit argument and
opening it through `task_platform::ConfinedRoot`
(`modules/task/source/task/platform/confined-file.hpp:31-60`), which exists
precisely so that "a path that is inspected and then opened by name" cannot be
two different objects.

It is worth naming what embedding costs, because the consumer paid it without
noticing. `registrationBytes` (`contract/provider.cpp:799-829`) *computes* the
registration document at run time by `std::format`, from the nine digests of the
files it pins. `ProjectSchemaOwner::create` then compares those digests against
the bytes handed in. Both sides of that comparison are derived from the same
`hashOf` call, so the comparison cannot fail — it is the ninth costume of the
defect catalogued in `docs/pitfalls/checks-that-cannot-fail.md`.

That diagnosis stands and its remedy moved. Q3 (§7.0) ruled that a project
author types no digest at all, so the party that computes the registration from
the files it just read is the framework's loader rather than a consumer's
`std::format`: the derivation is adopted, not retired. The comparison inside
each owner's `create` therefore stays an identity, and this document does not
claim that putting the registration on disk makes it capable of going red. What
becomes falsifiable is the comparison the loader does not sit on both sides of.
A stored session names a `project_registration_hash`; the directory is loaded
again later, and a directory whose bytes have moved derives a different one, so
the session is refused. Two independently produced values, and a check that can
fail. §7.0 records what that costs and what it does not cover.

## 1. `ProvidedProject`, member by member

`ProvidedProject` has thirteen members. Three of them are already files in
uf-chaos, two are files that exist under another name, five are authorities a
data-only project can never supply as values, two are host machinery that never
belonged to a project at all, and one is a document that does not exist anywhere
and has to be designed.

### 1.1 The five authorities

`provider.hpp:156-165` states why they are values:

> The five authorities are values rather than callables because that is how the
> Operator takes them: a deployment builds each one from the exact schema bytes
> its registration pinned, and the suite is then unable to mint a document any
> other way.

A data-only project can never supply any of the five, and the reason is
mechanical rather than a matter of taste. Every one has a private constructor
whose only reachable mint is framework code:

- `VerifiedProjectRegistration(ProjectRegistrationClaims, std::string,
  ContentHash)` is private at `modules/operator/source/operator/manifest.hpp:80-84`
  with `friend class ProjectRegistration;` at `:86`, and `:115-116` says "The
  sole mint for `VerifiedProjectRegistration`. There is deliberately no loose
  field spec and no API that canonicalizes caller-provided fields."
- `ProjectSchemaOwner(std::shared_ptr<State const>)` is private at
  `project-plugin.hpp:118`, with `friend class ProjectPluginHandle;` at `:120`;
  the only public route in is the static `create` at `:138-142`.
- `ProjectJournalSchemaOwner(ContentHash, JournalPayloadSchemaValidator)` is
  private at `journal-entry.hpp:77-80`; only `create` at `:88-92` is public.
- `ProjectToolCatalogSchemaOwner(...)` is private at `tool-invocation.hpp:143-147`;
  only `create` at `:155-159`.
- `ProjectReconcileSchemaOwner(...)` is private at `reconcile-outcome.hpp:87-91`;
  only `create` at `:97-101`.

So the project never could supply the values; what it supplies today is each
`create` call's arguments. Those arguments split cleanly in two. The **exact
schema bytes** are bytes and can be files. The **validator** is a
`std::function` — `CanonicalJsonValidator` and `ProjectDocumentValidator`
(`project-plugin.hpp:95-98`), `JournalPayloadSchemaValidator`
(`journal-entry.hpp:65-70`), `ToolCatalogValidator` (`tool-invocation.hpp:130-135`),
`ReconcileDispositionReader` (`reconcile-outcome.hpp:77-79`) — and a callable is
not bytes. **That callable is the entire file→C++ boundary, and it is the only
thing a directory cannot carry.**

The answer is not to ship a default validator. The consumer-onboarding
document's §6.3 correctly refused that: a framework default `ToolCatalogValidator`
"carrying its own idea of a catalog" would mean the consumer's registration names
a `tool_catalog_hash` the default never had to satisfy, which is the exact
failure `tool-invocation.hpp:150-153` exists to prevent. What replaces the
callable is the opposite move: **the framework supplies a generic JSON Schema
evaluator, and the project supplies the schema document the evaluator is pointed
at.** The authority does not move; it is bound tighter. Today uf-chaos's
`toolCatalogValidator` (`contract/provider.cpp:1152-1183`) is a hand-written
closure that *could* disagree with `schema/dream/tool-catalog-v1.json`; with the
catalog document read directly, a byte changed in the catalog changes what is
enforced, which is what `tool_catalog_hash` was always supposed to mean.

Applied to consumer-onboarding's §6.2 test — "for any seam, ask which hash inside
`project_registration_hash` pins the schema being applied" — nothing here fails
it. Every schema the framework's evaluator applies is one the registration
already pins (`ProjectRegistrationClaims`, `manifest.hpp:24-37`). The framework
supplies the engine; `ProjectRegistrationClaims` gains no member. Q3 changed who
writes those pins — the loader computes each one from the bytes it read — and
not which schema is applied, so the test is answered the same way and the answer
is worth less than it was: see §7.0.

uf-chaos already does exactly this, in the wrong tree. Its `documentValidator`
(`contract/provider.cpp:1053-1121`) parses the candidate and dispatches to a
compiled `JsonSchema` for every project-owned document; its
`journalPayloadValidator` (`:1123-1150`) selects a schema by event type and
returns that schema's real hash; its `toolCatalogValidator` (`:1177-1180`)
resolves the catalog row's `argument_schema` as a `$defs` name inside the
precondition schema. **The 26 schema documents are not decoration; they are
already applied.** What moves is the evaluator, not the enforcement.

### 1.2 The two observation recorders

`ProvidedProject::lastReduceInput` and `::lastDeriveInput` are
`std::shared_ptr<std::string>`, documented at `provider.hpp:192-202` as "Where
the deployment's document validator records the exact bytes it last saw as a
Reduce input."

They are written in exactly one place — inside the `ProjectDocumentValidator`
closure, at `contract/provider.cpp:1074` and `:1082` — and that closure is the
callable §1.1 just moved into the host. A data-only project therefore cannot
supply them for a reason stronger than "there is no file for it": the thing that
writes them stops being the project's code at all. They are host machinery that
was only ever on `ProvidedProject` because the validator was.

They leave the struct. The host's document validator owns the two recorders, and
the suite reads them from the loader's result. Two cases read them
(`conformance/source/suite-control-ledger.cpp:498` and `:536`); nothing else in
the tree does.

### 1.3 The table

| Member | What it is | Where it already exists in uf-chaos | What the host must do to obtain it from a directory |
|---|---|---|---|
| `registration` | `VerifiedProjectRegistration`; private ctor at `manifest.hpp:80-84`, sole mint `ProjectRegistration::verifyExact` (`:120-125`) | **nowhere, and under Q3 nowhere is where it stays** — the bytes are `std::format`ted at run time by `registrationBytes` (`contract/provider.cpp:799-829`), and that derivation moves upstream rather than becoming a file (§2.2) | hash the eight files and the artifact blobs the deployment block names; assemble the registration's canonical JCS from those digests and the block's `plugin_id` and `baseline_event_type`; compile the framework's own `schema/umbraflow-project-registration-v1.schema.json` and build `ProjectRegistrationSchemaOwner::create(sha256(schemaBytes), evaluator)` (`manifest.hpp:65-68`); extract `ProjectRegistrationClaims` from the validated document as `registrationClaims` does today (`provider.cpp:853-906`); call `verifyExact(bytes, sha256(bytes), owner)` |
| `schemaOwner` | `ProjectSchemaOwner`: three exact schema byte strings plus two callables (`project-plugin.hpp:105-110`, `:138-142`) | the three schemas are files: `schema/<deployment>/project-state-v1.schema.json`, `-/project-observation-v1.schema.json`, `-/tool-precondition-v1.schema.json` | read all three; supply the framework's RFC 8785 canonical validator and the framework's document validator, whose `function × direction` dispatch is fixed (§2.4) and whose project half is the three schemas |
| `journalSchemaOwner` | registration hash plus `JournalPayloadSchemaValidator` (`journal-entry.hpp:72-92`) | `schema/<deployment>/journal-manifest-v1.json` plus the 13 payload schemas under `schema/journal/` | read the manifest; verify each declared `sha256` against the file it names, as `loadPayloadSchemas` does (`provider.cpp:1005`); build a validator that selects by `namespaced_event_type`, validates, and returns that schema's computed hash |
| `toolCatalogSchemaOwner` | registration hash, catalog hash, `ToolCatalogValidator` (`tool-invocation.hpp:137-159`) | `schema/<deployment>/tool-catalog-v1.json` | read the catalog; refuse a row whose `argument_schema` is not a `$defs` name in the precondition schema (`provider.cpp:975`); validate arguments against that definition; return `ToolDescriptor` from the row's own `version`/`mutability`/`surface` |
| `reconcileSchemaOwner` | registration hash, manifest hash, `ReconcileDispositionReader` (`reconcile-outcome.hpp:81-101`) | `schema/<deployment>/reconcile-v1.schema.json` — but only as a *schema*; the input/output `$defs` names and the disposition mapping are C++ literals at `provider.cpp:1077-1080`, `:1096-1099`, `:1204-1224` | read a reconcile manifest (new, §2.4) naming the schema, the two `$defs` and the `value → ReconcileDisposition` map; validate; read the named member; map it |
| `pluginBytes` | the exact Luau source `plugin_sha256` pins | **nowhere** — `k_dreamPlugin` (`provider.cpp:142-215`) and `k_archivePlugin` (`:217-289`) are C++ raw string literals. (`annotate/plugin.toml` is *not* this: it is the offline annotator's label vocabulary, its own header says runtime code does not load it, and no `CMakeLists.txt` in either tree names it) | read a `.luau` file named by the manifest; `ProjectPluginRegistrar::registerPlugin` (`project-plugin.hpp:232-235`) already checks it against `registration.pluginHash()` |
| `artifactBlobs` | the named blobs `project_artifact_roots` pins (`provider.hpp:43-47` is emphatic that these are *not* the RuntimeArtifact) | **nowhere** — 34- and 38-byte C++ literals at `provider.cpp:1243-1244` and `:1320-1321` | read one file per named root; the registrar already verifies closure in both directions |
| `runtimeArtifact` | `ProjectRuntimeArtifact` — the model text plus every asset's bytes (`provider.hpp:51-55`) | `runtime/artifact/` — `page-model.toml`, `runtime-artifact.manifest.json`, nine PNGs under `assets/` | **nothing: the member dies.** The suite's `publishRuntimeArtifact` (`observation-fixture.hpp:139-178`) re-serializes a manifest from those bytes, so today the Host verifies a manifest the *suite* wrote. The loader hands the installer a path, the release carries the project's own manifest bytes, and `runtime_artifact_root_hash` becomes the project's own digest |
| `probeFrame` | one PNG. The `ProjectFingerprint` beside it (`provider.hpp:74-85`) is dropped by Q2 | `runtime/probe-frame.png` exists. The fingerprint does not: `contract/CMakeLists.txt:185-194` regexes `base_resolution` and `base_dpi` out of `page-model.toml` at configure time | read the PNG, and nothing else. Q2 ruled the geometry is published by `RuntimeModelBinding`, so no document restates it: `modules/task/runtime/model.luau:626-627` already publishes `base_resolution` and `base_dpi` into the same frozen table as the `declared_*_ids` at `:635-637`, and `runtime_model_finalize` (`modules/task/source/task/ffi/uf-tables.cpp:955-1002`) is the seam that carries them across. The extent check consequently cannot run in the loader — see §2.7 R8 |
| `lastReduceInput` | recorder written by the document validator (`provider.cpp:1074`) | **host machinery that never belonged to a project** | own it beside the host's document validator; the loader returns it |
| `lastDeriveInput` | the same, for Derive (`provider.cpp:1082`) | **host machinery that never belonged to a project** | as above |
| `vocabulary` | `ProjectVocabulary`, 16 fields (`provider.hpp:90-150`) | **nowhere — must become a new document** (§2.3) | read it, as strings, and refuse anything else |
| *(`ProjectRole`)* | which registration is being asked for (`provider.hpp:210-214`) | `schema/dream/` and `schema/archive/` are already two complete schema sets for two registrations | the directory declares its deployments by name and the conformance document says which plays which role (§2.2, §2.3) |

## 2. The project directory format

This is the deliverable that does not exist. uf-chaos's layout grew; the closest
thing to a specification of it is a `set(CHAOS_SCHEMA_FILES ...)` list in a
CMakeLists (`contract/CMakeLists.txt:23-50`). Below is that layout, written down
as a contract.

### 2.1 One fixed path, and everything else named

Exactly two paths are fixed, both at the project root:
`umbraflow-project.json` and `umbraflow-conformance.json`. Every other file is
named *by* those two, project-relative.

Naming rather than fixing the tree is a decision, and the reason is that the
framework already has the mechanism and the rule.
`ConfinedRoot::readFile`'s contract (`confined-file.hpp:52-55`) is:

> `relativeText` must already carry a validated manifest spelling: forward
> slashes, and no empty, `.` or `..` component. Refusing those is the caller's
> job because the caller knows what a manifest may say; this type only
> guarantees that what it opens is what it checked.

That is a manifest-names-paths design with a confinement guarantee already
implemented and already used by `RuntimeArtifactHandle`. A fixed tree would
force uf-chaos to move 26 documents for no gain, and would give the loader
nothing `ConfinedRoot` does not already give it.

### 2.2 `umbraflow-project.json` — the deployment manifest

This is the document *production* reads. It names one or more deployments and
the RuntimeArtifact they share, and each deployment block is that deployment's
registration stated as intent.

```json
{
  "schema": "umbraflow-project/v1",
  "runtime_artifact": "runtime/artifact",
  "primary_deployment": "dream",
  "deployments": [
    {
      "name": "dream",
      "plugin_id": "chaos.dream",
      "baseline_event_type": "project.baseline_created",
      "plugin": "plugin/dream.luau",
      "project_state_schema": "schema/dream/project-state-v1.schema.json",
      "project_observation_schema": "schema/dream/project-observation-v1.schema.json",
      "tool_precondition_schema": "schema/dream/tool-precondition-v1.schema.json",
      "tool_catalog": "schema/dream/tool-catalog-v1.json",
      "journal_event_schema_manifest": "schema/dream/journal-manifest-v1.json",
      "reconcile_manifest": "schema/dream/reconcile-manifest-v1.json",
      "artifact_blobs": [
        { "name": "page-model", "path": "blob/dream-page-model.blob" }
      ]
    }
  ]
}
```

Every member is required and `additionalProperties` is false. `deployments` has
`minItems: 1` and unique `name`s; `primary_deployment` must be one of them.

**There is no authored registration document, and that is Q3's doing.** The
seven schema and document members plus `plugin` correspond one-for-one to the
eight digests `ProjectRegistrationClaims` carries (`manifest.hpp:26-34`), and
`artifact_blobs` to its `project_artifact_roots`. Once no human types a digest,
a file that named those eight paths would name what the block above already
names, which is the second spelling this repository forbids; so the block *is*
the intent, and the loader derives the registration's canonical JCS from it and
from the digests of the files it read. `plugin_id` and `baseline_event_type` are
here because they are the two claims no file can supply.

The document the Operator verifies is therefore derived rather than opened, and
its shape is the framework's: `schema/umbraflow-project-registration-v1.schema.json`
already spells exactly `ProjectRegistrationClaims`' members, and its digest is
what `manifest_schema_hash` names. A project owns no registration schema either.

`runtime_artifact` names a directory, not a file: the installer already reads
`page-model.toml` and `runtime-artifact.manifest.json` from a root by those
fixed names (`modules/task/source/task/page-model-file.hpp:25-27`), so the
manifest names the root and the artifact's own manifest names its contents. One
RuntimeArtifact per project, shared by every deployment — which is what uf-chaos
already has: `runtimeArtifact()` and `probeFrame()` return byte-identical values
for both roles (`contract/provider.cpp:1447-1448`), because one model covers
both the `event` and `recruit` surfaces.

### 2.3 `umbraflow-conformance.json` — the roles, the probe frame, and the vocabulary

This is the document only the conformance binary reads. Production never opens
it, and that separation is the point: a `ProjectVocabulary` exists so a suite can
drive a project (`provider.hpp:86-88`), and nothing in production wants a "tool
the catalog does not carry".

```json
{
  "schema": "umbraflow-conformance/v1",
  "probe_frame": "runtime/probe-frame.png",
  "under_test": { "deployment": "dream",   "vocabulary": { … } },
  "foreign":    { "deployment": "archive", "vocabulary": { … } }
}
```

`probe_frame` is a path and nothing else. There is no `fingerprint` member:
Q2 ruled that `RuntimeModelBinding` publishes `base_resolution` and `base_dpi`,
so the extent this capture is checked against is the one the Host's trusted
parser read out of the model, never a number this document restates. What that
buys and what it costs is §2.7 R8.

Both roles are required, and both carry a vocabulary of their own, because three
cases reach the foreign one:
`conformance/source/suite-control-ledger.cpp:445` uses the foreign
`confirmedInput`, `suite-project-authority.cpp:134-137` its `mutatingTool` and
`toolArguments`, and `:147-150` its `confirmedEntry` and `provenance`.

**The vocabulary document.** The 16 fields of `ProjectVocabulary`
(`provider.hpp:90-150`) become 17 JSON members, snake_cased to match the
spelling the deployment block already uses (`plugin_id`,
`baseline_event_type`):

| Member | Type | What it means |
|---|---|---|
| `mutating_tool` | string | a name the Tool Catalog carries as `Mutating`. The suite submits it wherever it needs an Operation that changes something |
| `other_mutating_tool` | string | a second, different `Mutating` name. `contract-control-c13` needs two to prove the one-live-chain rule refuses whatever tool the second command names |
| `read_only_tool` | string | a `ReadOnly` name, so that mutability provably comes from the catalog and not from the caller |
| `tool_arguments` | string carrying exact JCS | arguments the catalog's argument schema accepts for `mutating_tool` |
| `refused_tool_arguments` | string carrying exact JCS | canonical bytes that same schema refuses. Together with the row above this is what makes the catalog's acceptance falsifiable |
| `absent_tool` | string | a name the catalog does not carry |
| `baseline_entry` | `{event_type, payload}` | the Journal entry a `ProjectInstance` is provisioned with. `event_type` must equal the registration's `baseline_event_type` |
| `progress_entry`, `confirmed_entry`, `superseded_entry` | `{event_type, payload}` | three more the suite appends. All four payloads must differ: the reducer-input case proves an entry a commit did not name never reaches the reducer, which is unprovable when two payloads are equal (`provider.hpp:102-107`) |
| `provenance` | string carrying exact JCS | one `JR:JournalProvenance` document. Its schema is the framework's and fixed (`journal-entry.hpp:55-64`); the values are the project's |
| `continue_input`, `confirmed_input`, `rejected_input`, `ambiguous_input` | strings carrying exact JCS | four reconcile inputs whose outputs this project's plugin maps to four of the five dispositions. Only the project can supply them, because its plugin decides the mapping (`provider.hpp:115-118`) |
| `approval_required_plan_tool` | string | one more `Mutating` name whose `PlanProposal` carries an effect whose risk requires human approval before the first dispatch. The proposal itself is deliberately absent: `freezePlan` calls `plugin.plan` itself, so a carried document would be a document nothing produced (`provider.hpp:123-134`) |
| `ui_action` | `{surface, ui_target, action}` | the one UI action a contract run drives against this project's RuntimeModel, in that model's own vocabulary (`provider.hpp:136-149`) |

Every payload member is a **JSON string whose content is the exact bytes**, not a
nested JSON object. This is the one non-obvious rule in the format and it is not
a convenience: those bytes are handed to `schemaOwner.canonicalize()`
(`conformance/source/suite-support.cpp:72`), which refuses anything that is not
exact RFC 8785 JCS. A nested object would make the *loader* choose a
serialization, and `provider.hpp:86-88` says the suite invents no project bytes.
A string keeps the project's bytes the project's, and makes the canonical
validator's refusal reachable from a document — which, per consumer-onboarding
§6.4 and prediction R11, it has never been.

### 2.4 Three project documents whose format becomes the framework's

The tool catalog, the journal event schema manifest and the reconcile payload
schema manifest are project-owned documents pinned by
`tool_catalog_hash`, `journal_event_schema_manifest_hash` and
`reconcile_payload_schema_manifest_hash`. Today uf-chaos invented all three
formats (`"schema": "uf-chaos-tool-catalog/v1"` and friends) because its own
provider read them. Once the host reads them, their format is the framework's,
and the framework ships a schema for each. Their *content* stays entirely the
project's.

Two of the three keep a stated `sha256` inside them, and Q3 does not reach it.
The rule that survives Q3 is narrow: **the loader computes every digest in the
document it derives, and computes none in a document the project authors.** It
derives the registration, so the registration's eight digests are its work; it
does not rewrite a project's files, so a digest inside one of those files stays
the author's. That is not a preference. The journal manifest's per-schema
`sha256` is the only path by which the 13 payload schemas' bytes reach
`journal_event_schema_manifest_hash`, and the reconcile manifest's is the only
path by which `reconcile-v1.schema.json`'s bytes reach
`reconcile_payload_schema_manifest_hash`; delete them and editing a payload
schema changes no hash anywhere, which is exactly the session refusal §0 just
relocated the falsifiability into. They are also the last digests in the design
a human types, so R5 is the last rule left that can go red on a mistyped one.

**Tool catalog** — `schema/umbraflow-tool-catalog-v1.schema.json`. uf-chaos's
existing document is already the right shape: `tools[]` of
`{name, version, mutability, surface, argument_schema}`, where `argument_schema`
is a `$defs` name resolved inside the deployment's tool-precondition schema. Two
members are dropped: `plugin_id` restates the registration, and
`tool_precondition_schema` restates the deployment block, and this repository
forbids two spellings of one fact. Both `mutability` and `surface` are
**required** on every row — `ToolDescriptor` defaults them to the restricted
value in C++ (`tool-invocation.hpp:58`, `:63`), and "absent means the safe
default" is still "absent means".

**Journal event schema manifest** — `schema/umbraflow-journal-manifest-v1.schema.json`.
uf-chaos's is already right: `payload_schemas[]` of
`{namespaced_event_type, path, sha256}`, and the provider already verifies each
`sha256` against the file it names (`contract/provider.cpp:1005`). `plugin_id`
is dropped for the reason above.

**Reconcile payload schema manifest** — `schema/umbraflow-reconcile-manifest-v1.schema.json`.
This one is new, because uf-chaos has no such document: it pins the reconcile
*schema* directly and keeps everything else in C++. The manifest carries the
schema and the four facts the C++ holds:

```json
{
  "schema": "umbraflow-reconcile-manifest/v1",
  "payload_schema": { "path": "schema/dream/reconcile-v1.schema.json", "sha256": "…" },
  "input_definition": "ReconcileRequest",
  "output_definition": "ReconcileVerdict",
  "disposition_member": "reconciliation",
  "disposition_map": {
    "Continue": "Continue", "Confirmed": "Confirmed", "Rejected": "Rejected",
    "Ambiguous": "Ambiguous", "Diverged": "Diverged"
  }
}
```

This replaces four hardcoded things: `"ReconcileRequest"`
(`contract/provider.cpp:1077-1080`), `"ReconcileVerdict"` (`:1096-1099` and
`:1200-1203`), the positional read `document.object().front().second.string()`
(`:1204`), and the five literal words (`:1205-1224`). `disposition_member` is a
**named** member rather than the positional read, deliberately: a positional read
means a later schema edit that adds a member silently changes which value is
read. `disposition_map` need not be exhaustive over the five — a project may
never produce `Diverged` — but every value in it must be one of the five, and a
document whose value is not a key is refused, as `:1225-1228` refuses it today.
uf-chaos's map happens to be the identity, and it is still written out, because
"absent means identity" is a default.

The one remaining hardcoded name, `ReconcileRequest` at the *input* seam,
becomes `input_definition`; nothing else in the document validator's dispatch is
project-specific. That dispatch is fixed and needs no data at all:

| Function | Direction | Who judges |
|---|---|---|
| Reduce | Input | framework reduce-envelope reader; each entry's payload by the journal manifest's schema for its event type |
| Derive | Input | framework derive-envelope reader; `project_state` and `prior_project_observation` by the project's two schemas |
| Plan | Input | framework plan-envelope reader; `canonical_args` by the catalog row's `argument_schema` |
| NextStep | Input | framework step-envelope reader |
| Reconcile | Input | the reconcile manifest's `input_definition` |
| Reduce | Output | the project's project-state schema |
| Derive | Output | the project's project-observation schema |
| Reconcile | Output | the reconcile manifest's `output_definition` |
| Plan | Output | `readPlanProposal` |
| NextStep | Output | `readStepIntent` |

That table is `contract/provider.cpp:1069-1119` with the project literals lifted
out. The last two rows are already framework readers today
(`conformance/include/conformance/operator-protocol.hpp:434`, `:589`) and
uf-chaos already calls them (`provider.cpp:1107`, `:1113`) — which is the
existing proof that the split works. `operator-protocol.hpp:27-33` states the
rule the other eight rows now follow:

> They belong to the suite rather than to a project provider because the operator
> protocol is the Operator's own schema and is the same for every project. What a
> provider supplies is the documents; what the Operator supplies is the reading
> of them. A consumer therefore writes a `ProjectVocabulary` and never a JSON
> reader.

### 2.5 Which files are JCS and which are ordinary JSON

**No file in a project directory is exact RFC 8785 JCS.** The one JCS document
in the design is still the registration, because
`ProjectRegistration::verifyExact` refuses anything else and its hash is the
project's identity; under Q3 the loader derives those bytes instead of opening
them, so JCS became a property of what the loader emits and never of what an
author types. Every JSON document in the directory — the two root manifests, the
three project manifests, the schemas — is ordinary UTF-8 JSON, which is what
uf-chaos's schemas already are (two-space indent, `$comment` members). The
vocabulary's payload *strings* carry JCS because the framework hands them to
`canonicalize()`; the file containing them does not.

Stating that is not tolerance of two spellings. It is one rule with one reason:
a document is JCS exactly when something hashes it and compares. That is true of
exactly one document, and no human writes it.

### 2.6 The format is schema-validated, by the framework's own schemas

Yes, and by the same evaluator that validates project payloads. Five new
framework schemas live beside the eight already in `schema/`:
`umbraflow-project-v1`, `umbraflow-conformance-v1`, `umbraflow-tool-catalog-v1`,
`umbraflow-journal-manifest-v1`, `umbraflow-reconcile-manifest-v1`.

A sixth framework schema starts binding without being written, and it is already
in the tree: `schema/umbraflow-project-registration-v1.schema.json` spells
exactly the members of `ProjectRegistrationClaims`. Because Q3 made the
registration a document the loader derives, its shape is the framework's, and
that file is what `manifest_schema_hash` names (`manifest.hpp:26`, `:102`).
uf-chaos's `schema/registration-v1.schema.json` — the *project's* schema for the
*project's* registration, whose own `description` says the Operator never reads
it — has nothing left to describe and dies with `contract/`. The
directory-format schemas are framework-owned for the reason consumer-onboarding
§6.2 gives: no member of `ProjectRegistrationClaims` pins a directory layout,
and none would be added, because a layout is not something a project's identity
attests to.

The evaluator's closed keyword set applies to the framework's schemas too:
`compile()` refuses any keyword it does not implement
(`modules/json/source/json/schema.hpp:20-25`), and the set is data a test can
pin rather than prose (`:83-87`). A framework schema that used
`unevaluatedProperties` would fail to compile at load rather than be silently
under-enforced.

### 2.7 Missing and malformed

There is no "absent means the default" reading anywhere. Eight rules, each a
refusal that names what it read. Seven and a half are the loader's; the half
R8 keeps outside it is Q2's bill, and R8 says why:

- **R1.** Both root documents are required at their fixed names. Absent → refusal
  naming the absolute path and the two names.
- **R2.** Every member of every framework-owned document is `required`, and every
  object sets `additionalProperties: false`. A missing member and an unknown
  member are both refusals. No member has a default.
- **R3.** Every path member is a manifest spelling in `ConfinedRoot`'s sense
  (`confined-file.hpp:52-55`): forward slashes, no empty, `.` or `..` component.
  Checked before anything is opened.
- **R4.** Every named file must exist and be a regular file inside the confined
  root. Missing → refusal naming both the manifest member and the path. This is
  the directory form of `REQUIRE(found != k_schemaFiles.end())`
  (`contract/provider.cpp:311`), and it is only equivalent if the loader errors
  on a missing file rather than skipping it.
- **R5.** Every stated `sha256` must equal the digest of the bytes it names.
  After Q3 that is two documents rather than three: the journal manifest's
  per-schema digests and the reconcile manifest's `payload_schema.sha256`
  (§2.4). The registration states none, so there is nothing about it to check
  here — its digests are the loader's own arithmetic. Disagreement → refusal
  printing the stated digest, the computed digest and the path. This is the only
  rule of the eight that a mistyped digest can make go red, which is the whole
  of what R5 is now for.
- **R6.** Every schema must compile under the evaluator's closed keyword set.
  A keyword the evaluator does not implement is a refusal, not a skip
  (`json-schema.hpp:9-16`, enforced today by
  `REQUIRE_MESSAGE(schema.has_value(), path)` at `contract/provider.cpp:330`).
- **R7.** Both `mutability` and `surface` are required on every catalog row; a
  row omitting one is refused rather than read as `Mutating`/`Privileged`.
- **R8.** Of the two cross-document agreements `prepareStore` checks today, one
  moves into the loader and one cannot. `baseline_entry.event_type` must equal
  the deployment block's `baseline_event_type` (`suite-support.cpp:162-165`):
  both halves are authored, so the loader refuses them where they were written.
  The probe frame's decoded extent must be the model's (`requireProbeGeometry`,
  `observation-fixture.hpp:233-254`, called at `suite-support.cpp:171`) — and
  after Q2 the model's extent does not exist until the Host has activated the
  artifact, which a loader does not do. That check stays in the conformance run
  and moves *later* rather than earlier, to just after `activateObservationHost`
  (`observation-fixture.hpp:540-554`), where the binding first exists. It is the
  one rule below with no case in `tests/deployment/`, and it is what Q2 cost: a
  directory can no longer be refused for a probe frame its own model does not
  fit until something installs and activates the artifact.

One consequence worth stating rather than discovering. `provider.cpp` is a
doctest translation unit: it includes `<doctest/doctest.h>` and its ~40
`REQUIRE`s outside any `TEST_CASE` are counted by
`uf_require_executed_assertions` (`cmake/conformance-suite.cmake:256`). A loader
returning `Result<LoadedProject>` collapses those to one assertion in the suite
— `REQUIRE_MESSAGE(loaded.has_value(), loaded.error().message())` — which is a
loss only if the forty were ever falsified, and they were not: nothing anywhere
tests that a malformed uf-chaos directory is refused. The forty become unit
cases in `tests/deployment/`, one per rule above except R8's second half, each
red when its rule is removed. That is a strict gain and it is why the rules are
enumerated here.

## 3. The binary

**Decision: a second shipped binary, `umbra-flow-conformance`, built from
framework sources only, taking the project directory as an argument. Not a
subcommand of `umbra-flow`.**

The weaker argument first, so it is not mistaken for the reason. doctest is a
real cost inside a product binary, and it is not confined to `conformance/source/`:
both exported headers include `<doctest/doctest.h>`
(`host-delivery-fixture.hpp:15`) and assert inside ordinary functions — 28
occurrences in `observation-fixture.hpp`, 11 in `host-delivery-fixture.hpp`,
several inside `static` members of friend classes (`:150-168`). A subcommand
would put doctest's static registration, its argument parser and its summary
protocol into a binary whose stdout is already a parsed protocol
(`entry/cli/main.cpp:52-57`), and whose gate would then match on doctest's
summary line (`cmake/doctest-gate.cmake:26`). That is a real cost. It is not
decisive, because it could be paid.

The decisive argument is what the two binaries do. `umbra-flow` exists to reach a
live target: its two subcommands are `explore` and `targets`
(`entry/cli/main.cpp:96-99`), and the whole product invariant above it is that it
acts on a real window in the background. The conformance run is structurally
incapable of that and must stay so: its frame source and its action sink both
declare `TargetWorld::Recorded` (`observation-fixture.hpp:355` and `:436`), and
the comment at `:436-439` says why that pairing exists —

> It must agree with `ObservationFrameSource` above or `EngineSession::create`
> refuses the session, which is the check that keeps a recorded capture from ever
> driving a sink that posts for real.

Merging them puts a code path that must never reach a target inside the binary
whose entire job is reaching one, with a subcommand string as the only thing
between them. Two binaries make that separation a link-time fact.

What the second binary costs is one target and one `main`, and `319bdb1` made
that literal: `entry/` now holds `main.cpp` and nothing else, because everything
the commands are made of became `uf::cli` (`entry/CMakeLists.txt:10-16`). A
second entry point is a second directory beside it. It is not a second copy of
anything: the loader, the five authorities, the modules and the suite sources
are shared, and the entry point is the only file that is not. The honest
cost is that a release now carries two executables and a consumer has to be told
which to run — paid in documentation, not in code.

One argument must **not** be given for the split, because it will stop being true:
that `umbra-flow` does not link `uf::operator` and would grow SQLite, stb, Luau
and the ONNX payload. It does not link it today (`entry/CMakeLists.txt:22`,
which names `uf::cli` and `uf::core` and nothing else), but
the decision in §0 says a project runs as a plugin inside umbraflow, so the
product binary will link the Operator. The separation must rest on the recorded
world, not on the link graph.

**doctest's home.** It is vendored at `tests/external/doctest` with **no CMake
target at all**, spelled by hand in three places: `tests/CMakeLists.txt:31`,
`cmake/conformance-suite.cmake:83`, and — from another repository —
`E:\umbraflow-projects\uf-chaos\runtime\CMakeLists.txt:44`. A test framework that
a *shipped* binary compiles against cannot keep living under `tests/`. It moves
to `external/doctest` with one `uf::doctest` INTERFACE target defined beside
`uf_require_executed_assertions` in `cmake/doctest-gate.cmake`, which both
`tests/` and the conformance binary already include. Three hand-written paths
become one `target_link_libraries`, and the third disappears with uf-chaos's
`runtime/`.

Calling it a shipped *test runner* rather than a shipped *product* is what makes
that consistent: doctest's cost lands in the binary whose job is running tests,
and nowhere else.

**Argument surface.** `umbra-flow-conformance --project <dir> [--frames <corpus>]`,
with every remaining argument passed through to doctest — so per-case selection
stays `--test-case=contract-control-c01` and no second spelling of doctest's
filter is invented. `main` is `DOCTEST_CONFIG_IMPLEMENT` rather than
`…_WITH_MAIN`; uf-chaos's `runtime/check/main.cpp:17` already does exactly this
against these headers, so the shape is proven.

**How the directory reaches the cases** is the one admission this design owes.
doctest gives a `TEST_CASE` no parameters, so the path is a process-scope value
set once by `main` before `context.run()` and never written after — declared in
`conformance/source/`, not in a public header, and read through a function rather
than a variable. Cases run single-threaded in one process
(`suite-support.hpp:22-24` already relies on that for temporary directories).
Q9 asks whether that is acceptable.

**CTest registration.** `cmake/conformance-suite.cmake` (277 lines) is replaced by
`cmake/conformance-run.cmake` with `uf_add_conformance_run(PROJECT <name>
DIRECTORY <dir> CASES …)`, which registers `add_test(NAME conformance-<name>
COMMAND umbra-flow-conformance --project <dir>)` plus one per claimed case. It
keeps the two checks that earn their keep — the CASES-versus-declared-`TEST_CASE`
cross-check (`conformance-suite.cmake:110-160`) and
`uf_require_executed_assertions` — and drops everything about compiling a
consumer's sources, applying a safety profile to them, staging the ONNX payload
and checking that provider files exist, because there are none.

On the assertion count: the brief for this design cites 5,787 assertions across
the 15 cases. That number is recorded nowhere in this tree, and nothing enforces
an absolute figure — `cmake/doctest-gate.cmake:26` enforces only that the count
is not zero. Nothing below depends on it.

## 4. What moves, what dies, what is new

### 4.1 Moves upstream

"Moves upstream" is a convenient heading and a slightly wrong description of the
1,547 lines in `contract/json-schema.cpp` and `contract/json-value.cpp`. They are
not a consumer's invention being adopted by the framework. **They are the
consumer implementing something the framework's own header assigns to the host,
and had left unimplemented.** `project-plugin.hpp:90-94`, directly above the two
`std::function` typedefs:

> These validators are trusted deployment code. The canonical validator must
> reject anything other than exact RFC 8785 JCS. The document validator must
> validate the complete function-specific JSON Schema, including every
> project-owned nested payload. Neither callable is passed to plugin code or
> published in a business VM.

That paragraph is a specification of host work. The seam
(`ProjectSchemaOwner::create`, `:138-142`) was always host-side; there was simply
no host, so the only party able to satisfy it was the project, and uf-chaos wrote
1,547 lines to do it. Two things follow. The code lands wherever the host builds
the arguments it passes to `create` — that is a destination decision, made below,
not a promotion of the consumer's version. And nothing about the consumer's
implementation is authoritative: it is one deployment's answer to a question the
deployment layer owns, and the framework re-deciding any of it is the framework
taking back what the header already said was its.

**`E:\umbraflow-projects\uf-chaos\contract\json-value.{hpp,cpp}` (88 + 719) →
`modules/json/source/json/value.{hpp,cpp}`. Landed at `a0ae304`.** The argument
below was written for `core` and is kept, because it is the record of why this
facility is the framework's work at all; `evaluate-core-capability` then refused
`core` as a destination on size and fallibility rather than on that argument
(§7.0 Q1). The argument for `core` is the one `core` already makes about itself,
at `modules/core/source/core/text/json-text.hpp:15-19`:

> It lives in core because three components write JSON that a fourth reader
> compares byte for byte … A second spelling of this transform cannot fail a
> test — it produces bytes that merely disagree.

`core` held two of RFC 8785's four rules — the string escape (`json-text.hpp:20`)
and member-name ordering (`:40-42`) — and
`docs/reviews/2026-08-10-third-round-review.md:493-499` recorded that number
formatting and container framing had no C++ implementation because they need a
value tree. `canonicalBytes` and `requireExactCanonical`
(`modules/json/source/json/value.hpp:114`, `:121`) are that missing half, and
`tests/vectors/jcs-vectors.txt` was already in this tree as the cross-language
oracle. **The fallback this document named — `modules/deployment/`, beside the
schema evaluator — was not available, and the reason is one this document
missed: `deployment` sits above `operator` and `task`, where two of the four
hand-rolled readers the facility replaces live
(`modules/json/manifest.txt:9-14`). A JSON reader placed above either could
never serve them.** The module sits on `core` alone, which is the lowest place
that serves every caller, and `core`'s own JCS story is now complete rather than
half-written.

**`contract/json-schema.{hpp,cpp}` (53 + 828) →
`modules/json/source/json/schema.{hpp,cpp}`. Not `core`, and landed beside the
value tree at `a0ae304` rather than in `deployment`.** The destination was
argued, not defaulted, and the half of the argument that kept it out of `core`
still holds. A JSON Schema 2020-12 evaluator is not a primitive: it is a policy
engine over one draft, and its keyword set is deliberately closed and incomplete
(`modules/json/source/json/schema.hpp:20-25`; the implemented set is data a test
can pin, `:83-87`). Putting a closed-world evaluator in `core` invites a second
consumer to widen it, and `core` is the module `scripts/check_modules.py` forbids
from declaring dependencies. What moved is the other half — "it belongs beside
its only caller" was written when the value tree was going to `core` and the
evaluator could not follow it there. The value tree went to `modules/json`
instead, the evaluator's only dependency is that tree, and the two are one
module.

**The four Operator envelope readers →
`modules/deployment/source/deployment/operator-envelope.{hpp,cpp}`.**
`requireExactMembers` (`contract/provider.cpp:549-577`), `validateReduceInput`
(`:595-647`), `validateDeriveInput` (`:649-679`), `validatePlanInput`
(`:681-723`), `validateStepInput` (`:725-744`) and the null passthrough
`validateNullableDocument` (`:579-593`) judge documents `ledger.cpp` assembles
itself; there is no schema file for them anywhere and no project knowledge in the
question. `readPlanProposal` and `readStepIntent`
(`operator-protocol.hpp:434`, `:589`) are the same class of thing and move to the
same place, with the suite including them from there rather than owning them.
This closes consumer-onboarding's Q1 by making it moot: nobody writes an envelope
reader because nobody writes a provider.

**Registration assembly and extraction →
`modules/deployment/source/deployment/project-registration.{hpp,cpp}`.** Three
functions, not two, and the third is what Q3 added. `registrationBytes`
(`contract/provider.cpp:799-829`) hashes the files a deployment names and
`std::format`s the registration from those digests; under Q3 that is no longer a
consumer's shortcut around a document it did not want to maintain, it is the
only way the registration is ever produced, so it moves here and its output is
JCS by construction rather than by a comment (§2.5). `registrationClaims`
(`:853-906`) then maps the validated members onto `ProjectRegistrationClaims`,
and `namedHash` (`:831-846`) prefixes `"sha256:"` because the registration
spells bare hex and `ContentHash::parse` wants the algorithm. All three were
framework work in a consumer's tree.

**`E:\umbraflow-projects\uf-chaos\runtime\check\main.cpp` (587) → the conformance
binary's `--frames` mode.** Its own comment (`runtime/check/main.cpp:10-13`)
already says the install-and-parse half is covered by the suite and the frame
corpus is what is not. A project with no C++ cannot keep it, and deleting it
would delete the only thing that drives the model over real captures.

### 4.2 Dies

In this repository:

- `conformance/include/conformance/provider.hpp:219-220` — `provideProject`,
  and with it `ProjectRole` as an exported type (`:210-214`).
- The exported include directory itself. `conformance/include/` folds into
  `conformance/source/`, and `conformance/manifest.txt:3-9` stops describing
  "first-party C++ on a consumer's include path" while `:18-22` stops resting
  its public dependency list on what a consumer compiles against.
- `ProjectRuntimeArtifact` (`provider.hpp:51-55`) and `ArtifactFile` (`:31-35`),
  and with them `artifactManifestRow` (`observation-fixture.hpp:126-134`) and
  `publishRuntimeArtifact` (`:139-178`) — 60 lines that re-serialize a manifest
  the project already published.
- `ProvidedProject::lastReduceInput` and `::lastDeriveInput` (`:196-202`).
- `cmake/conformance-suite.cmake`, all 277 lines.
- `conformance/exemplars/umbraflow/provider.cpp` (95),
  `conformance/exemplars/arcana-expedition/provider.cpp` (1,063), and both
  `CMakeLists.txt`.
- The project-construction half of
  `conformance/exemplars/umbraflow/project-fixture.hpp` (1,804 total) —
  everything from `registrationBytes` through `makeProject` and the four owner
  `create` calls (`:387-404`, `:413`, `:525`, `:589`, `:700`). Its store
  machinery moves to `tests/support/`, because `tests/operator/*` includes it and
  that dependency is not what this correction is about.

In `E:\umbraflow-projects\uf-chaos`:

- `contract/` entirely: `provider.cpp` (1,462), `json-schema.cpp` (828),
  `json-value.cpp` (719), `json-value.hpp` (88), `json-schema.hpp` (53),
  `CMakeLists.txt` (269), `README.md` (401).
- The root `CMakeLists.txt` (60), whose only job is `add_subdirectory` of the
  framework.
- `schema/registration-v1.schema.json`, the twenty-sixth schema document. Q3
  left it nothing to validate: the registration is derived by the loader and
  the framework's own `schema/umbraflow-project-registration-v1.schema.json` is
  what judges it (§2.6).
- `runtime/CMakeLists.txt` (79) and `runtime/check/main.cpp` (587), replaced by
  `--frames`.
- Both generated headers and the machinery that writes them —
  `chaos-schema-bundle.generated.hpp` and `chaos-runtime-bundle.generated.hpp`
  (`provider.cpp:59-60`), produced by `contract/CMakeLists.txt:52-103` and
  `:196-259`. Every file that exists only to cross the file→C++ boundary is in
  this group, including the CMake regex that reads `base_resolution` out of TOML
  (`:185-194`).

**Stated plainly, because it is the whole point: a consuming project contains no
C++ at all.** `contract/` has no successor. It is deleted, not relocated — the
1,547 lines of JSON machinery (1,688 with their headers) are replaced by a host
implementation of a seam that was always the host's (§4.1 opening), and the
remaining 2,132 — `provider.cpp`, the CMake that generates the two bundle
headers, and the README explaining both — exist only to cross a boundary that
stops existing. When step 7 completes, uf-chaos has
no compiler, no CMake, no build directory and no target: it is documents, Luau,
PNGs and a RuntimeArtifact, and the only executable it needs is one this
repository ships.

### 4.3 Is new

- **`modules/deployment/`** — the module the framework's own headers keep naming
  and has never had. "Trusted deployment code" (`project-plugin.hpp:90`), "the
  supplying deployment" (`provider.hpp:86-88`), "a deployment builds each one from
  the exact schema bytes its registration pinned" (`:157-159`). Its content: the
  manifest reader, the directory loader, the envelope readers, the registration
  assembly and extraction, and the construction of all five authorities. Not the
  JSON Schema evaluator, which landed in `modules/json` (§4.1). Dependencies:
  `core domain json task operator` public, plus `image` for the probe frame.

  §0.1's call-site census is the evidence that it does not exist: no shipped
  binary registers a plugin or mints an authority, and
  `docs/plans/2026-08-10-w2-effective-plan.md:1185-1189` says so in as many
  words. This module is that deployment, and the conformance binary is one of its
  callers rather than its purpose — which is the reason it is a module rather
  than something under `conformance/`.

- **Five framework schemas** in `schema/`: `umbraflow-project-v1`,
  `umbraflow-conformance-v1`, `umbraflow-tool-catalog-v1`,
  `umbraflow-journal-manifest-v1`, `umbraflow-reconcile-manifest-v1`. A sixth,
  `umbraflow-project-registration-v1`, is not new — it is already in `schema/`
  and starts binding rather than starts existing (§2.6).
- **`entry/conformance/main.cpp`** and the `umbra-flow-conformance` target.
- **`external/doctest`** and the `uf::doctest` INTERFACE target.
- **`cmake/conformance-run.cmake`**.
- **Two in-tree exemplar directories**, replacing the two provider translation
  units, and remaining what `conformance/CMakeLists.txt:1-6` already calls them:
  documentation a consumer copies.
- In uf-chaos: `umbraflow-project.json`, `umbraflow-conformance.json`, two
  `plugin/*.luau` files carrying the bytes that are today C++ raw literals, two
  artifact-blob files, and two reconcile manifests. No registration document:
  Q3 put the intent in the deployment block and the digests in the loader
  (§2.2). Everything else it needs it already has.

## 5. Migration order

Eight steps. Every step leaves this tree building and `scripts/ci-local.ps1`
green. Where a step breaks the consumer, it is said so explicitly.

**1. Rule on `evaluate-core-capability` for a JSON value tree, then land it —
done, at `a0ae304`.** The skill refused `core`, so the parser, the value tree,
`canonicalBytes` and `requireExactCanonical` landed as `modules/json` on `core`
alone, with `tests/vectors/jcs-vectors.txt` as the oracle and a case per vector
(`tests/json/test-value.cpp`, `tests/json/test-schema.cpp`). Nothing consumes it
yet. It took the JSON Schema evaluator with it, which was half of step 2 as
written, and step 2 is correspondingly lighter.

**2. Land `modules/deployment/` with the envelope readers, and publish the
model's geometry on `RuntimeModelBinding`.** Two framework capabilities that
nothing consumes yet, which is what makes them one step. `readPlanProposal` and
`readStepIntent` move out of `operator-protocol.hpp` and the suite includes them
from their new home. Alongside them, Q2's half of the framework: the trusted
parser already publishes `base_resolution` and `base_dpi` into the frozen table
(`modules/task/runtime/model.luau:626-627`), so what is added is a wider
`runtime_model_finalize` arity and a `ProjectFingerprint` on the binding beside
the `DeclaredRuntimeUi` (`modules/task/source/task/ffi/uf-tables.cpp:955-1002`,
`modules/task/source/task/page-model-file.hpp:178-221`). It lands here rather
than with the loader because it belongs to `task`, and it must land before step 6
or the loader has no fingerprint to give the suite. Unit tests only.

**3. Land the five framework schemas and the directory loader.** The loader
returns `Result<LoadedProject>`, derives each deployment's registration from its
block and the digests of the files it read, and constructs all five authorities.
Its tests are `tests/deployment/`, one case per rule R1–R8 except R8's second
half, each proven red by removing its rule. Still nothing consumes it:
`provideProject` is untouched and the suite runs exactly as before.

**4. Move doctest to `external/doctest` and define `uf::doctest`.** Three
hand-spelled paths become one. Mechanical, and independently revertible.

**5. Write uf-chaos's directory documents, in the consumer tree, changing
nothing else there.** Two `plugin/*.luau` files, two artifact blobs, two
reconcile manifests, the reshaped tool catalogs and journal manifests, and the
two root documents — `umbraflow-project.json` carrying both deployment blocks,
which is where the registrations went (§2.2). `contract/` still builds and
`conformance-chaos` still passes; nothing reads the new files. **This step is
before the framework switch on purpose**: uf-chaos's build points at the
framework source root (`E:\umbraflow-projects\uf-chaos\CMakeLists.txt:19-21`), so
the moment step 6 lands it stops configuring, and the window in which the project
is unverifiable is exactly the gap between step 6 and step 7. Writing the data
first makes that gap one commit wide.

**6. Switch, in one change.** Both in-tree exemplars become directories; the
suite calls the loader; `provideProject`, `ProjectRole` as an exported type, the
two recorders, `ProjectRuntimeArtifact`, `ProjectProbeFrame::fingerprint`,
`cmake/conformance-suite.cmake`, both `provider.cpp` files and both exemplar
`CMakeLists.txt` are deleted; `umbra-flow-conformance` and
`cmake/conformance-run.cmake` arrive. `requireProbeGeometry` moves from the top
of `prepareStore` (`suite-support.cpp:171`) into `activateObservationHost`
(`observation-fixture.hpp:540-554`), just after the Host has parsed the model and
before the `ObservationRuntime` that needs the same fingerprint is built — still
before any resolution, so `requireResolvedSurface`'s account of which causes can
reach it (`observation-fixture.hpp:576-582`) survives the move and its wording
does not. It is one change rather than two because a tree in which some projects
arrive as C++ and some as data is two spellings of one thing, and `CLAUDE.md`
forbids resting there.

The evidence that the switch is lossless is assembled in the working tree and not
committed, and Q3 and Q6 changed what that evidence can be. It cannot be the
registration hash computed both ways: the C++ side spells its members
`plugin_sha256` and `registration_schema_sha256`
(`contract/provider.cpp:799-829`), the derived side spells them `plugin_hash` and
`manifest_schema_hash` (`schema/umbraflow-project-registration-v1.schema.json`),
so the two documents are different bytes on purpose and their digests are
expected to differ. What must match is the *claims*: `plugin_id`,
`baseline_event_type`, the artifact roots, and each of the eight digests,
computed both ways and compared member by member, before the C++ side is
deleted. `tool_catalog_hash` is expected to differ too, by exactly the members
Q6 dropped. After the deletion there is nothing left to compare against, which
is why it is a falsification and not a compatibility path.

Two constraints this step must satisfy or the gate goes red for unrelated
reasons. `tests/CMakeLists.txt:68-128` requires the CTest names
`contract-control-c01`, `-c06`, `-c09`, `-c10`, `-c11`, `-c12` and `-c13` to
exist, and they are registered today by
`conformance/exemplars/umbraflow/CMakeLists.txt:15-21`; `uf_add_conformance_run`
must register the same seven. And `CMakeLists.txt:66-72` includes the suite
CMake outside the `PROJECT_IS_TOP_LEVEL` guard so a consumer's
`add_subdirectory` reaches it — with no consumer `add_subdirectory` left, the
whole include moves inside the guard.

**7. Delete uf-chaos's `contract/`, its root `CMakeLists.txt` and its generated
headers — but only after `umbra-flow-conformance --project
E:/umbraflow-projects/uf-chaos` has run green.** Not earlier, and specifically
not as step 1: until the host can load a project directory, deleting the provider
leaves uf-chaos with no way to be verified at all, and a project that cannot be
verified is a project whose registration nobody can trust. This is the step the
ordering exists to protect.

**8. Delete `runtime/CMakeLists.txt` and `runtime/check/main.cpp` once
`--frames` reproduces what they did.** Last, because it is the only remaining
C++ in the consumer and it is the only one whose replacement cannot be checked
by CI here — the screenshot corpus `assets/screens/` is gitignored and exists
only on the annotation machine (`runtime/CMakeLists.txt:60-63`).

## 6. What this correction does not touch

It is worth being generous here, because the list of what survives is longer
than the list of what changes, and a reader who thinks the 15 cases are being
rewritten will resist the whole design.

**The 15 cases assert the right things and 13 of them do not change at all.**
Only how the project reaches them changes. The two that change are the two that
read the recorders — `conformance/source/suite-control-ledger.cpp:498` ("the
reducer is handed exactly the Journal prefix that is appended") and `:536` ("the
deriver is handed an envelope no caller could have supplied") — and they change
by reading the recorder from the loader's result instead of from
`ProvidedProject`. Every assertion in both stays. The three sites that call
`provideProject(ProjectRole::Foreign)` — `suite-control-ledger.cpp:433`,
`suite-project-authority.cpp:129` and `:166` — ask the loader for the second
deployment instead. The other twelve reach the project only through
`prepareStore` and `prepared.project.vocabulary.*`, which keep their shape.

**Seven of the 42 `REQUIRED_CORE` requirements have their behavioural gate inside
the suite**, and those seven are the whole of the project-acquisition rewiring:
`C-01`, `C-06`, `C-09`, `C-10`, `C-11`, `C-12`, `C-13`, declared at
`conformance/source/suite-control-ledger.cpp:23, 49, 83, 125, 155, 247, 349` and
registered at `conformance/exemplars/umbraflow/CMakeLists.txt:15-21`. The count is
checkable: `tests/CMakeLists.txt:178-184` asserts the matrix has 42
`REQUIRED_CORE` entries; 33 of the 40 `contract-*` gates live in `tests/`, all 19
`schema-*` gates live in `tests/operator/`, and `A-03` and `A-05` own no
per-requirement contract gate (`tests/CMakeLists.txt:192-195`). So 35
requirements' gates are not touched in any way, and the remaining seven keep
their assertions and change only where their project comes from.

**Twenty-five of uf-chaos's 26 schema documents are kept and are, for the first
time, the thing that decides.** They are already applied today
(`contract/provider.cpp:1039-1183` runs a real evaluator against them); what
changes is that the evaluator lives upstream and the bytes arrive from the files
rather than from a `constexpr` copy of them. Twenty-one are byte-identical: the
13 journal payload schemas under `schema/journal/`, both
`project-state-v1.schema.json`, both `project-observation-v1.schema.json`, both
`tool-precondition-v1.schema.json` and both `reconcile-v1.schema.json`. Four
change shape and only by losing restated members: the two tool catalogs and the
two journal manifests drop `plugin_id`, and the catalogs drop
`tool_precondition_schema` (§2.4). One dies —
`registration-v1.schema.json`, which Q3 left nothing to validate (§2.6).

**The RuntimeArtifact is kept and is verified more strictly.** `page-model.toml`,
`runtime-artifact.manifest.json` and the nine PNGs under
`runtime/artifact/assets/` are correct data and are not touched. What improves is
that the Host verifies the project's own manifest bytes: today the suite
recomputes a manifest from the assets (`observation-fixture.hpp:139-178`), so
`runtime_artifact_root_hash` is a digest of bytes the *suite* wrote, and
uf-chaos has to pin the real one by hand
(`contract/provider.cpp:363-365`) to notice a disagreement.

**The composed probe frame is kept.** `runtime/probe-frame.png` is a real
capture at the model's own geometry and nothing about it changes. Only where its
fingerprint comes from moved: Q2 ruled it comes from the model, published by
`RuntimeModelBinding`, so no document restates it and `contract/CMakeLists.txt`'s
TOML regex has nothing left to do. The check that the capture is that extent
survives and runs later rather than earlier (§2.7 R8).

**The Luau plugin sources are kept byte for byte.** They move from
`contract/provider.cpp:142-215` and `:217-289` into two files, and
`plugin_sha256` becomes the digest of a file rather than of a C++ literal, which
is the whole point of the exercise.

**Untouched entirely:** `tasks/*.luau` (the project's task layer),
`annotate/plugin.toml` (the offline annotator's label vocabulary — no build file
in either tree names it), `assets/`, `schema/journal/`, and everything under
`docs/architecture/`. Also untouched here: the Operator's ledger, the Host, the
engine, the trusted Luau runtime, and every one of the 33 `contract-*` and 19
`schema-*` gates in `tests/`.

## 7. Open questions

Ten. Each carries a recommendation and says what it blocks. **All ten are now
ruled in §7.0; the questions below are kept as the argument each ruling came
from, not as anything still open.**

### 7.0 Rulings, 2026-08-11

The owner delegated every one of them with a single criterion: minimise the work
for the person writing a project and for the person maintaining this framework,
and do not buy safety with elaborate checking. Clear, correct and safe is the
bar.

**Q1 and Q4 — settled by measurement rather than by ruling.**
`evaluate-core-capability` refused `core`: 1,675 lines over 22 files, largest
file 203, and no header outside `error/` returning `Result` or `Status` at all.
The fallback this document named, `deployment`, is unavailable for a reason this
document missed — it sits above `operator` and `task`, where two of the readers
the facility replaces live. It landed as `modules/json` on `core` alone
(`a0ae304`). `deployment` keeps the name for the loader.

**Q2 — yes: `RuntimeModelBinding` publishes the geometry.** A base resolution is
not semantics. `EngineSession::ensureCompatibleFrame` already refuses a capture
whose extent disagrees with a fingerprint, so the framework already compares
these two numbers and has never interpreted them; the only question was where
the fingerprint comes from. A human retyping a number the model already states
is pure work and pure drift, and this document has already collected two places
where both trees wrote down that the restatement is a hazard.
`umbraflow-conformance.json` carries no `fingerprint` member.

**Q3 — nobody maintains them, because the document stops carrying them.**
Neither option offered was right. Hand-maintaining nine SHA-256 digests is the
most error-prone thing this design could ask of a project author, and computing
them from the files just read is the vacuous check §4 objected to. Both are
wrong for one reason: in a directory there is exactly one source of bytes, so a
digest written beside the file it describes is a second spelling of one thing.

The registration document declares *intent* — which plugin, which schemas, which
artifact roots, each by path — and the loader computes every hash from the bytes
it read, including `project_registration_hash`. The spec's `ProjectRegistration`
keeps its shape: `plugin_id + plugin_hash`, the tool catalog, the artifact
roots. What changes is only that no human types a hash.

The check that matters survives, and it can still fail, because it compares two
independently produced things: a stored session names a
`project_registration_hash`, and a directory whose bytes have changed produces a
different one, so the session is refused. That answers the question anyone
actually has — *did this project change under a running session* — where a
per-file pin answered one nobody asked. No authoring verb is added.

*Amended 2026-08-11 after an independent review, which found a decisive argument
this ruling did not have and reversed one of its assumptions.*

**A registration document is not something a human can author at all.** It must
be exact RFC 8785 JCS: `ProjectRegistration::verifyExact` accepts nothing else,
and `ProjectRegistrationSchemaOwner` refuses any other serialization. Nobody
hand-maintains canonical bytes with sorted keys — the first thing any author
would do is write a script to emit them, and a script-emitted Option A **is
Option B with a ceremony in front of it**. The evidence is already on disk: the
only real project in the ecosystem, at
`E:\umbraflow-projects\uf-chaos\contract\provider.cpp:780-829`, already computes
every digest from disk bytes and assembles the document in JCS key order, with
no hash from a human hand. Designing for A would force each project to bring a
tool that turns A into B.

**And Option A is the one more likely to add a check that cannot fail**, which
is the opposite of how it reads. Its regeneration script hashes the same disk
bytes the loader is about to read, so the comparison degrades to
`hashOf(x) == hashOf(x)` — silently, while keeping the appearance of an
attestation. B's version of that emptiness is at least visible and named.

Two consequences to carry into implementation:

- **`verifyExact`'s `expectedRootHash` is a real check only when it comes from
  another time or another source** — the ledger, or a signature. Passing the
  value just computed, on a first load, is legal and empty. The call site says
  which case it is in.
- **The one check with teeth must be proven to have them.** Against a stored
  session naming hash `H`, flip one byte of a pinned file in the project
  directory, reload, and resume: the refusal must fire and must print both
  hashes. If that does not go red, there is no real check anywhere on this
  chain, and the whole ruling is unfounded. It is a work item, not a hope.

The review's one stated uncertainty, recorded rather than resolved: during a
first load, with no stored session, a mid-load modification is undetectable. Its
judgement, which this ruling accepts, is that nothing is owed there — no prior
commitment exists to violate — and that aligning a first load with an external
expectation is the signature design's job, not this one's.

**Q5 — yes, after step 6, as its own change.** As recommended.

**Q6 — drop the restated members.** Nothing is released. Under Q3 the loader
recomputes every hash regardless, so uf-chaos's recorded values move once rather
than twice.

**Q7 — refuse a directory with one deployment.** As recommended: a skipped case
is a green result promising more than it verified.

**Q8 — keep `--frames`.** It is not new machinery. It is the same loader pointed
at a directory of captures instead of at one probe frame, and it is the only
thing that drives a model over real ones. Dropping it would delete a real check,
and would require the project's host to keep a C++ build alive in order to keep
it — the opposite of what this document is for.

**Q9 — one function in `conformance/source/`.** As recommended.

**Q10 — recorded, not ruled, and it blocks nothing.** A recognition gap is a
framework work item with the project's measurement attached, never a C++ hatch
in a project.

**Reconciled, 2026-08-11, against `319bdb1`.** §0, §1.1, §1.3, §2.2–§2.7, §3,
§4.1–§4.3, §5 and §6 were amended in place so that no section still describes
the shape a ruling replaced. What moved: the registration document became the
deployment block plus a document the loader derives (Q3); the conformance
document now says in as many words that it carries no `fingerprint`, and where
the geometry comes from instead (Q2); the three project manifests lost their
restated members (Q6); §4.1's two
destination arguments became history, because the value tree and the evaluator
both landed as `modules/json`; and §5's steps 1, 2, 3, 5 and 6 were rewritten
around what has landed and what Q2 added. Citations that the tree had moved
under were corrected: `entry/cli/main.cpp:51-56` and `:95-98` to `:52-57` and
`:96-99`, `entry/CMakeLists.txt:95` to `:22`, `conformance/manifest.txt:17-23`
to `:3-9` and `:18-22`, `tests/CMakeLists.txt:190-193` to `:192-195`, and the
`contract/json-*.hpp` citations to their `modules/json` homes. Two counts were
wrong before any ruling touched them and are fixed: §2.2's "seven" digests are
eight, and §6's "twenty-three byte-identical, three reshaped" is twenty-one,
four and one deleted.

Six things the reconciliation turned up, recorded rather than edited around.

**Q3 leaves a project with no authored registration document at all.** Once no
human types a digest, a registration file would state the same paths the
deployment block already states, which is the second spelling `CLAUDE.md`
forbids. So the block is the intent and the loader derives the registration.
uf-chaos writes two fewer new documents than §4.3 promised, deletes one schema
more than §4.2 listed, and the framework's already-shipped
`schema/umbraflow-project-registration-v1.schema.json` starts binding.

**Q3's surviving check works under one reading of the ruling and fails silently
under the other.** `VerifiedProjectRegistration::hash()` is the SHA-256 of the
registration's canonical JCS and nothing else
(`modules/operator/source/operator/manifest.cpp:302-304`). If the loader hashes
the *authored* intent document, then editing a pinned schema changes no byte in
it, `project_registration_hash` does not move, and the session refusal the
ruling rests on — "a directory whose bytes have changed produces a different
one" — cannot fire. It fires only if the loader derives the document that
carries the digests and hashes *that*. §0 and §2.2 are written that way, which
is the only reading under which Q3 justifies itself.

**The price of that reading is the defect §0 opened with.** With the loader on
both sides, the comparison inside each owner's `create` is an identity by
construction — the ninth costume, now worn deliberately by the framework instead
of accidentally by a consumer. §0 no longer claims that putting the registration
on disk makes the pin check capable of going red, because it does not. The
falsifiable check is the session comparison, and it is the only one.

**Q3 must stop at the registration or it removes coverage.** Its reason — a
digest written beside the file it describes is a second spelling — applies
verbatim to the journal manifest's per-schema `sha256` and the reconcile
manifest's `payload_schema.sha256`. Applying it there would put the 13 journal
payload schemas and `reconcile-v1.schema.json` outside every hash in the design,
because those two digests are the only path by which their bytes reach
`project_registration_hash`. §2.4 states the narrow rule that keeps them and
§2.7 R5 is what is left of the rule that checks them.

**Q2 makes §2.7 R8's second half unenforceable at load time.** R8 promised that
both cross-document agreements move into the loader, "refused where they are
authored rather than where they are used". The probe frame's extent cannot: the
fingerprint does not exist until the Host has activated the artifact, which a
loader does not do. It moves *later* instead, into `activateObservationHost`,
and a project directory can no longer be refused for a probe frame its own model
does not fit without installing the artifact first. That is what deleting the
restated number cost. It is still worth it — a restatement that can drift buys
nothing — but R8 no longer claims the check is free.

**Q2 does not trip `tests/test-runtime-surface.py`, which was checked rather
than assumed.** Its trusted-parser rule scans first-party C++ for
`RuntimeModelParser`, `parseRuntimeModel`, `parseRuntimeModelEnvelope` and
`toml::parse`, and counts the trusted Luau parsers
(`tests/test-runtime-surface.py:205-213`, `:418-447`); carrying two numbers the
trusted parser already computed introduces none of them and adds no parser. The
comment that could have objected — `DeclaredRuntimeUi`'s "a reader may ask
whether a name is in one of them, which is identity, and cannot ask what the
name means" (`modules/task/source/task/page-model-file.hpp:159-167`) — is
unaffected, because the fingerprint is a sibling of `DeclaredRuntimeUi` on the
binding rather than a fourth vocabulary inside it.

One consequence of Q3 and Q6 together lands in §5. Step 6's evidence that the
switch is lossless was "each exemplar's registration hash and
`tool_catalog_hash`, computed both ways, must match". Neither can match now: Q6
deliberately changes the catalog bytes, and Q3 changes the registration's member
spellings from `plugin_sha256` to `plugin_hash`. The evidence is a member-by-
member comparison of the claims instead, and step 6 says so.

**Q1 — does `core` gain a JSON value tree and the complete RFC 8785
canonicaliser?** *Recommend yes, gated on `evaluate-core-capability`, which
`CLAUDE.md` requires before anything is promoted.* §4.1 gives the argument and the
fallback. **Blocks:** step 1, and therefore every later step; and, if refused,
whether `core`'s two-of-four RFC 8785 rules stay two of four forever.

**Q2 — does `RuntimeModelBinding` publish the model's `base_resolution` and
`base_dpi` as a `ProjectFingerprint`?** *Recommend yes.* The alternative is that
the conformance document restates the geometry, and both trees have already
written down that this is a hazard: `conformance/exemplars/arcana-expedition/provider.cpp:205-208`
("one fact stated twice, and must not drift: C++ parses no RuntimeModel, so
nothing but this comment holds them together") and
`E:\umbraflow-projects\uf-chaos\contract\CMakeLists.txt:178-184`, which built the
TOML regex specifically to stop restating it. The restatement is also
uncheckable: a document stating the true extent while the model states another
resolves nothing, and `requireResolvedSurface` can only print the resolution
(`observation-fixture.hpp:576-582`). The mechanism exists —
`model.luau:626-627` already publishes both values into the same frozen table as
`declared_surface_ids` (`:635-637`), which already crosses the private native
surface into `DeclaredRuntimeUi` (`page-model-file.hpp:158-172`). **The
counter-argument is real:** `DeclaredRuntimeUi`'s comment says a reader "may ask
whether a name is in one of them, which is identity, and cannot ask what the name
means", and `tests/test-runtime-surface.py` enforces that the Operator reads no
RuntimeModel semantics. Whether a base resolution is identity or semantics is the
ruling. **Blocks:** whether `umbraflow-conformance.json` carries a
`fingerprint` member, and a small reordering in `prepareStore` (the fingerprint
would be available only after the Host activates the artifact, so
`requireProbeGeometry` moves after `activateObservationHost`).

**Q3 — who maintains the registration document's nine digests?** Today they
cannot be wrong because they are computed (`contract/provider.cpp:799-829`), and
that is exactly why the framework's pin check cannot fail. *Recommend
hand-maintained, with the loader's refusal printing the stated digest, the
computed digest and the path, so fixing it is a copy and no tool is needed.*
**Blocks:** whether the framework grows an authoring verb that rewrites a
registration, which would be a new kind of thing for this binary to do.

**Q4 — is the module called `deployment`?** *Recommend yes*, because it is the
framework's own word for the thing that has never existed
(`project-plugin.hpp:90`, `provider.hpp:86-88`, `:157-159`), and because `project`
is already three concepts. **Blocks:** the paths in §4 and nothing else.

**Q5 — does `tests/operator/` also load the in-tree exemplar directory?** If it
does, `conformance/exemplars/umbraflow/project-fixture.hpp`'s project half dies
outright instead of moving; if it does not, roughly 770 lines of the same job
survive in `tests/`, which is the third spelling consumer-onboarding §3 already
counted. *Recommend yes, but as a separate change after step 6*, because it
touches the Operator's own tests and would make step 6 unreviewable. **Blocks:**
the size of step 6 and whether the third spelling survives the migration.

**Q6 — do the three project manifests keep their restated members?** Dropping
`plugin_id` from the catalog and the journal manifest, and
`tool_precondition_schema` from the catalog, changes `tool_catalog_hash` and
`journal_event_schema_manifest_hash`, therefore `project_registration_hash`,
therefore every session identity uf-chaos ever creates. *Recommend dropping
them*: nothing is released, and a cross-check between two documents one hash
already covers is a second spelling rather than a check. **Blocks:** whether step
5 changes uf-chaos's registration hash, which its own documents record.

**Q7 — what does a project with one deployment do?** The suite needs two
registrations that can each mint documents, and `provideProject` guarantees that
by construction. *Recommend that `umbraflow-conformance.json` requires both
`under_test` and `foreign`, and that a directory with one deployment is refused
rather than running a reduced suite* — a skipped case is a green result promising
more than it verified. **Blocks:** whether a minimal consumer can onboard with a
single registration, which is the first thing a third project will ask.

**Q8 — does the conformance binary keep a `--frames` mode?** *Recommend yes.*
uf-chaos's `runtime-model-check` is the only thing that drives the model over
real captures, and its host is a machine with a gitignored corpus. The
alternative is deleting a check because its home repository may no longer compile
C++. **Blocks:** step 8, and whether the corpus check survives at all.

**Q9 — is a process-scope project path acceptable?** doctest gives a `TEST_CASE`
no parameters, so `main` must set the directory somewhere the cases can read it.
*Recommend one function in `conformance/source/`, set once before
`context.run()` and never after, with the value returned by copy* — the suite
already assumes one process and one thread (`suite-support.hpp:22-24`). **Blocks:**
§3's entry-point design; if refused, the alternative is a doctest reporter or a
per-case fixture that re-reads an environment variable, both of which are worse.

**Q10 — what happens when a project's recognition needs something
`modules/vision` or `modules/ocr` cannot do?** This is the one pressure point
that will be used to argue the correction back out, so the answer is recorded
here before it is asked. **It is a host capability gap, to be filled in the host,
and never a C++ escape hatch in the project.** The design admits no such hatch
and none is possible: the plugin runs in `PureDataProgram`'s VM behind a 23-name
global allowlist (`ffi/pure-data-program.cpp:63-76`) whose header says "No host
installer or native capability seam is part of this API"
(`pure-data-program.hpp:17-18`), and the tasks above it reach the world only
through the trusted Luau runtime's verbs. A project that could link C++ would be
able to reach a target directly, which defeats the background-only product
invariant that no compiled contract case can detect — the shape
`docs/plans/2026-08-11-consumer-onboarding.md:886-902` already identified as
belonging to attestation rather than to a shipped check. *Recommend that a gap be
recorded as a framework work item with the project's measurement attached, and
that the project wait.* **Blocks:** nothing today, and it is written down
precisely so that the next reader who hits a missing verb does not reinvent the
hatch instead of filing the gap.

## 8. Index entries

This document registers itself nowhere; `docs/INDEX.md` and
`docs/plans/README.md` are not edited by this change. For `docs/INDEX.md`, after
the consumer-attestation entry:

```markdown
- [A project is a directory of data](plans/2026-08-11-project-as-data.md) — the
  correction that follows from "uf-chaos depends only on umbraflow's compiled
  binary": the exported C++ provider surface is replaced by a project directory
  format and a host-side loader, and conformance becomes a second shipped
  binary. A consuming project ends with no C++ at all. All ten questions are
  ruled in §7.0; step 1 of the migration has landed as `modules/json`.
```

For `docs/plans/README.md`, beside the consumer-onboarding entry:

```markdown
- [A project is a directory of data](2026-08-11-project-as-data.md) —
  **specification, ruled; step 1 landed.** `ProvidedProject` member by member and
  which of its thirteen members a data-only project can never supply; the project
  directory format, its two root documents and the vocabulary document that does
  not exist today; why conformance is a second binary rather than a subcommand;
  why the plugin mechanism is already finished and the loader is the only missing
  piece; what dies in both trees, and the eight-step order in which uf-chaos's
  `contract/` may finally be deleted, leaving a project with no compiler and no
  CMake. §7.0 rules all ten questions and records what reconciling §2, §4 and §5
  with them exposed: no project writes a registration document, and a probe
  frame's geometry can no longer be checked at load time.
```
