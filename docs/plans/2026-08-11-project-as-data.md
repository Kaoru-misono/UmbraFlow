# A project is a directory of data, not a C++ library

Status: specification, ruled, and being built. All ten questions are answered in
§7.0, and §2, §4 and §5 below are written as those rulings left them. Steps 1 to
3 of §5 have landed — `modules/json` (`a0ae304`), `modules/deployment` with the
envelope readers and the deployment's validators (`8277fe3`, `b6c7e72`,
`b9ef6e7`) and the model's published geometry (`eb238cb`), and the directory
loader with the framework's own schemas for the format. §2 has been amended in
place wherever building it decided something §2 had left open; each such place
says so.
Date: 2026-08-11
Scope: `umbraflow-cpp`, plus a statement of what the correction costs
`E:\umbraflow-projects\uf-chaos`. Both trees were read only; no file outside
this one was written and no git command that writes was run in either.
Framework read at `6bfe1d6` and reconciled against `319bdb1`; consumer read at
`95d9668` with a dirty tree.

Related: [consumer onboarding](../archive/plans/2026-08-11-consumer-onboarding.md) measured what
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
`docs/archive/plans/2026-08-10-w2-w7-reconciliation.md`'s predecessor already recorded as
"no production deployment exists yet"
(`docs/archive/plans/2026-08-10-w2-effective-plan.md:1185-1189`).

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
(`modules/conformance/source/conformance/suite-control-ledger.cpp:498` and
`:536`); nothing else in
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

> Amended 2026-08-12: only the first is required of a project. The second is
> read by `deployment::loadConformanceProject` and by nothing else, so a project
> that ships no conformance fixture holds no such file and the product still
> starts it. See R1 in §2.7.

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
      "reconcile_schema": "schema/dream/reconcile-v1.schema.json",
      "tool_catalog": "schema/dream/umbraflow-tool-catalog-v1.json",
      "journal_event_schema_manifest": "schema/dream/umbraflow-journal-manifest-v1.json",
      "reconcile_manifest": "schema/dream/umbraflow-reconcile-manifest-v1.json",
      "journal_payload_schemas": [
        "schema/journal/project.baseline_created-v1.schema.json"
      ],
      "effect_payload_schemas": [
        "schema/dream/effect-run-v1.schema.json"
      ],
      "artifact_blobs": [
        { "name": "page-model", "path": "blob/dream-page-model.blob" }
      ]
    }
  ]
}
```

Every member is required and `additionalProperties` is false, with the single
`$comment` exception §2.7 R2 states. `deployments` has `minItems: 1` and unique
`name`s; `primary_deployment` must be one of them.

**There is no authored registration document, and that is Q3's doing.** Once no
human types a digest, a file that named these paths would name what the block
above already names, which is the second spelling this repository forbids; so
the block *is* the intent, and the loader derives the registration's canonical
JCS from it and from the digests of the files it read. `plugin_id` and
`baseline_event_type` are here because they are the two claims no file can
supply.

**How the block's ten file-naming members reach the registration's eight
digests, which is not one-for-one.** An earlier reading of this section said
"the seven schema and document members plus `plugin` correspond one-for-one to
the eight digests `ProjectRegistrationClaims` carries (`manifest.hpp:26-34`)",
and recounting does not repair it — the correspondence is not one-for-one in
either direction:

- Seven members reach a digest directly, as the bytes it is the sha256 of:
  `plugin`, `project_state_schema`, `project_observation_schema`,
  `tool_precondition_schema`, `tool_catalog`, `journal_event_schema_manifest`
  and `reconcile_manifest`.
- The eighth digest, `manifest_schema_hash`, corresponds to no block member and
  cannot: it names
  `schema/umbraflow-project-registration-v1.schema.json`, the framework's own
  published document, which every project shares and none supplies (§2.6).
- `reconcile_schema` reaches the registration only through
  `reconcile_schema_sha256` inside the reconcile manifest, and
  `journal_payload_schemas` only through that manifest's per-entry digests
  (§2.4). Both are one link longer than the seven above, and R5 is what holds
  each link.
- `effect_payload_schemas` reaches it through the tool catalog's
  `effect_payload_sha256s`, which is the same one-link-longer shape and is held
  by the same rule.
- `artifact_blobs` is not a digest at all: it becomes
  `project_artifact_roots`, a list of `{name, root_hash}` beside the eight.

**An effect payload schema's bytes reach `project_registration_hash` through the
tool catalog, and through nothing else.** The catalog carries a required
`effect_payload_sha256s` array beside `tool_precondition_sha256`, which is
already that document's precedent for attesting a schema file it does not
otherwise name, and the loader holds it to the deployment's
`effect_payload_schemas` as a set in both directions, exactly as it does the
journal manifest's. It is not the deployment block, because a digest beside the
path of the file it describes is the one thing Q3 forbids everywhere, and it is
not a new registration member, because `ProjectRegistrationClaims` and the
published registration schema are the framework's identity for every project.

*Landed 2026-08-11, one file larger than this section had counted.*
`k_toolCatalogSchema` is a closed object, so the framework schema had to declare
the member and list it in `required` in the same change as
`ProjectDeployment::create`'s set check, the four catalogs under
`examples/*/schema/*/tool-catalog-v1.json`, the two generators at
`conformance/exemplars/umbraflow/project-schemas.hpp` and
`conformance/exemplars/arcana-expedition/project-schemas.hpp`, and uf-chaos's
two authored catalogs at
`schema/{dream,archive}/umbraflow-tool-catalog-v1.json`. Both of uf-chaos's
registration hashes moved with it — `d5d2246…` to `1bee72c…` for `dream` and
`59cf758…` to `9e7f828…` for `archive` — and no stored session recorded either.

**`artifact_blobs` may be empty**, and the section that says no member has a
default should say so as plainly as it says it of `effect_payload_schemas`. A
deployment that registers no artifact root is a deployment whose plugin is
handed no blob; the member is still required, so the author writes `[]` and no
reader infers one from an absent member.

**Three members this section did not originally have, and could not do without.**
`ProjectDeploymentSources` (`modules/deployment/source/deployment/project-deployment.hpp:73-110`)
takes the reconcile schema, one JSON Schema per journal payload and one per
`OP:ExpectedEffect` payload, and none of the three is reachable from the block
this section first published: the reconcile manifest names its schema by
`sha256` and never by path, the journal manifest does the same for each payload
schema, and the tool catalog does the same for each effect payload schema. So
the block names the files and the documents that pin them name the bytes. `journal_payload_schemas` has
`minItems: 1` and `effect_payload_schemas` may be empty, because a project that
proposes no effect is a project with nothing to pin — but a plugin that emits
an `OP:ExpectedEffect` naming a digest this list does not supply is refused,
which is a real bill for uf-chaos (§4.3).

**A project file's name is never load-bearing.** The block names every file by
path, so nothing in the format reads a filename, and two files whose contents
differ may sit beside each other under any two names. That answers the collision
step 5 hits: `contract/CMakeLists.txt` embeds the old-format catalogs and
journal manifests by exact path, so the reshaped documents cannot share those
paths while `contract/` still builds. The `umbraflow-` prefix uf-chaos used is
ratified as the step-5 spelling, and it is an expedient rather than a rule:
after step 7 nothing embeds anything and the names are the author's again.

**Artifact roots.** `name` is the registration's own `root_name`
(`^[a-z][a-z0-9_-]*(\.[a-z][a-z0-9_-]*)*$`), `path` is an ordinary
project-relative path, and there is no fixed `blob/` directory. The author
writes them in any order and the loader sorts them into the JCS order
`validateClaims` requires (`manifest.cpp:143-163`) and refuses a repeated name.
Sorting rather than requiring a sorted list is the same decision as deriving the
digests: an order is not something a project has an opinion about.

The document the Operator verifies is therefore derived rather than opened, and
its shape is the framework's: `schema/umbraflow-project-registration-v1.schema.json`
already spells exactly `ProjectRegistrationClaims`' members, and its digest is
what `manifest_schema_hash` names. A project owns no registration schema either.

`runtime_artifact` names a directory, not a file: the installer already reads
`runtime-model.toml` and `runtime-artifact.manifest.json` from a root by those
fixed names (`modules/task/source/task/runtime-model-file.hpp:25-27`), so the
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

The loader does decode it (§2.7 R9). That the extent cannot be checked at load
is not a reason to leave *whether there is an image at all* unchecked: a
`probe_frame` naming a JSON document loaded silently until 2026-08-11, and the
member's own type — a capture of the project's target — is the loader's to
enforce even when the number it must match is not yet in existence.

Both roles are required, and both carry a vocabulary of their own, because three
cases reach the foreign one:
`modules/conformance/source/conformance/suite-control-ledger.cpp:445` uses the
foreign
`confirmedInput`, `suite-project-authority.cpp:134-137` its `mutatingTool` and
`toolArguments`, and `:147-150` its `confirmedEntry` and `provenance`.

`under_test.deployment` and `primary_deployment` may name different deployments
and nothing checks that they agree, because they answer different questions:
one is which deployment production runs by default and lives in the document
production reads, the other is which registration a conformance run drives and
lives in the document production never opens. Requiring them to agree would put
a conformance fact into the production manifest. What is checked is that each
names a declared deployment and that `under_test` and `foreign` do not name the
same one — which is Q7's refusal, stated where both halves are written.

**The vocabulary document.** The 17 fields of `ProjectVocabulary`
(`provider.hpp:87-150`) become 17 JSON members, snake_cased to match the
spelling the deployment block already uses (`plugin_id`,
`baseline_event_type`). The mapping is one-for-one; an earlier count of 16
fields here was wrong, and the thirteen rows below name all seventeen:

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

**Seven of those rows are claims about another document in the same directory,
so the loader checks them and does not take the project's word.** `mutating_tool`,
`other_mutating_tool` and `approval_required_plan_tool` must name tools the
deployment's Tool Catalog carries as `Mutating`, `read_only_tool` one it carries
as `ReadOnly`, `absent_tool` one it carries not at all, `other_mutating_tool`
must differ from `mutating_tool`, and the four entry payloads must differ from
one another. Each is a case that runs green while proving nothing when the claim
is false, and `absent_tool` is the sharpest: a directory whose `absent_tool` is a
carried tool turns the catalog's refusal — the one thing that field exists to
make falsifiable — into a pass with nothing red anywhere. All of them were
accepted until 2026-08-11; see §2.7 R8, which is the rule they belong to and
whose criterion they meet.

Every payload member is a **JSON string whose content is the exact bytes**, not a
nested JSON object. This is the one non-obvious rule in the format and it is not
a convenience: those bytes are handed to `schemaOwner.canonicalize()`
(`modules/conformance/source/conformance/suite-support.cpp:72`), which refuses
anything that is not
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

All three landed at `8277fe3`, as schema documents inside
`modules/deployment/source/deployment/project-deployment.cpp`, and what landed
differs from what this section first proposed. The landed shapes are the ones
below, because they are the ones that decide.

**Each of the three is shown here as one complete document, and that is a
correction.** When this section was reconciled with the rulings, the three
examples it carried were rewritten into prose member lists — with the
consequence that the format's only complete statement became a C++ string
constant. The first person to write documents against the corrected text wrote
uf-chaos's six by reading `k_toolCatalogSchema` and its two neighbours, and
guessed `Mutating`/`Semantic` for what those constants spell `mutating` and
`semantic`. A format a consumer cannot write without opening the framework's
sources is not written down. §2.6's argument that a second copy under `schema/`
would be a second spelling stands and is untouched: a copy under `schema/` is an
input nothing reads, while an example inside the specification is prose with a
test behind it. What stops it from drifting is
`tests/deployment/test-project-directory.cpp`, which extracts each example
below by its anchor and requires `deployment::validateFrameworkFormat` — the
same schema bytes `ProjectDeployment::create` compiles — to accept it. An
example that stops being a document this framework accepts is red.

**Tool catalog** — `"schema": "umbraflow-tool-catalog/v1"`, an object of
`{$comment?, effect_payload_sha256s[], plugin_id, schema,
tool_precondition_sha256, tools[]}` where each
row is `{argument_schema, mutability, name, surface, version}` and
`argument_schema` is a `$defs` name resolved inside the deployment's
tool-precondition schema. `mutability` is `mutating` or `read_only` and
`surface` is `semantic` or `privileged`, both lowercase, both **required** on
every row — `ToolDescriptor` defaults them to the restricted value in C++
(`tool-invocation.hpp:58`, `:63`), and "absent means the safe default" is still
"absent means".

<!-- example: umbraflow-tool-catalog/v1 -->
```json
{
  "$comment": "chaos.click is absent on purpose: the design refuses a universal click, which is why the conformance vocabulary's absent_tool has a value at all.",
  "schema": "umbraflow-tool-catalog/v1",
  "plugin_id": "chaos.dream",
  "tool_precondition_sha256": "ddcbf99944a2ecd02b63c549afdc1b7a038126776cc8a990b4450df78209b2c0",
  "effect_payload_sha256s": [
    "2eb9225d5bf5b9a694158b0b456e8df6512669ad4b50f90f936a0ab0f1615598"
  ],
  "tools": [
    {
      "name": "chaos.choose_event_option",
      "version": "1",
      "mutability": "mutating",
      "surface": "semantic",
      "argument_schema": "ChooseEventOptionArguments"
    },
    {
      "name": "chaos.get_battle_state",
      "version": "1",
      "mutability": "read_only",
      "surface": "semantic",
      "argument_schema": "ReadBattleStateArguments"
    }
  ]
}
```

Q6 said to drop `plugin_id` and `tool_precondition_schema` as restatements. The
first half is overruled and the second is answered differently. `plugin_id`
stays and is checked against the deployment's: a catalog that answered for
whichever registration presented it would be a catalog no registration owns,
which is a property of the document rather than a copy of the block. And the
path `tool_precondition_schema` is replaced by the digest
`tool_precondition_sha256`, which is not a restatement of anything — it is the
only route by which the precondition schema's bytes reach `tool_catalog_hash`.

**Journal event schema manifest** —
`"schema": "umbraflow-journal-event-schema-manifest/v1"`, named for the
registration member that pins it. It carries `{$comment?, payload_schemas[],
plugin_id, schema}` and each entry is `{namespaced_event_type, sha256}` —
**and no `path`**. Which file carries which payload schema is the deployment
block's `journal_payload_schemas`; which schema answers for which event type is
this document's, decided by digest. The two never appear side by side, which is
what makes the question "what is a manifest `path` relative to" have no answer
rather than a badly chosen one.

The two halves are a set on each side and the loader compares both directions
(§2.7 R8): a digest no supplied file hashes to is refused, and a supplied file
no entry names is refused as well. The second direction is not symmetry for its
own sake — a payload schema no entry names reaches
`journal_event_schema_manifest_hash` through nothing, so it would sit in the
directory outside every digest in the design.

<!-- example: umbraflow-journal-event-schema-manifest/v1 -->
```json
{
  "$comment": "No entry names a path: which file carries which schema is umbraflow-project.json's journal_payload_schemas, so no fact is written down twice.",
  "schema": "umbraflow-journal-event-schema-manifest/v1",
  "plugin_id": "chaos.dream",
  "payload_schemas": [
    {
      "namespaced_event_type": "project.baseline_created",
      "sha256": "f2da2aa9749b9c8cbf327e44c4c7992a8bebedbda4d450ab256562c6cafa8c63"
    },
    {
      "namespaced_event_type": "battle.completed",
      "sha256": "3d6714b1b5dff0f9a1443ef49f22d8773512e7ba327ba1b5f1e4eecbae997d8b"
    }
  ]
}
```

**Reconcile payload schema manifest** — `"schema": "umbraflow-reconcile-manifest/v1"`,
carrying `{$comment?, dispositions[], plugin_id, reconcile_schema_sha256,
request_definition, schema, verdict_definition, verdict_member}`. It replaces
four hardcoded things: `"ReconcileRequest"` (`contract/provider.cpp:1077-1080`),
`"ReconcileVerdict"` (`:1096-1099` and `:1200-1203`), the positional read
`document.object().front().second.string()` (`:1204`), and the five literal
words (`:1205-1224`). `verdict_member` is a **named** member rather than the
positional read, deliberately: a positional read means a later schema edit that
adds a member silently changes which value is read. `dispositions` is a list of
`{disposition, value}` rather than a map, so that the framework's five words are
the closed vocabulary on one side and the project's words are free text on the
other; it need not be exhaustive over the five — a project may never produce
`diverged` — and a verdict whose value is in no entry is refused. A project
whose words happen to be the framework's still writes them out, because "absent
means identity" is a default.

<!-- example: umbraflow-reconcile-manifest/v1 -->
```json
{
  "$comment": "chaos.dream answers with `reconciliation` and chaos.archive with `settlement`: two deployments that agreed on the spelling could not prove that a document minted under one is refused by the other.",
  "schema": "umbraflow-reconcile-manifest/v1",
  "plugin_id": "chaos.dream",
  "reconcile_schema_sha256": "178029a53a3acc8a56a12b01e50821283c1c02bcf3bb8c2e85759a11c5b6008d",
  "request_definition": "ReconcileRequest",
  "verdict_definition": "ReconcileVerdict",
  "verdict_member": "reconciliation",
  "dispositions": [
    {"disposition": "continue", "value": "Continue"},
    {"disposition": "confirmed", "value": "Confirmed"},
    {"disposition": "rejected", "value": "Rejected"},
    {"disposition": "ambiguous", "value": "Ambiguous"},
    {"disposition": "diverged", "value": "Diverged"}
  ]
}
```

**The four schema identities.** A deployment's project-state, project-observation,
tool-precondition and reconcile documents must each declare the `$id` the
framework's own envelope schemas reference them by —
`https://umbraflow.dev/schema/project/{state,observation,tool-precondition,reconcile}`
(`project-deployment.hpp:57-65`). Two *deployments* may declare the same four,
and the two a project directory requires necessarily do: uf-chaos is one project
whose `dream` and `archive` deployments declare identical `$id`s, and that is
the configuration the loader actually runs. Each deployment's schemas are
compiled into a closed set of their own, so an identity is only ever resolved
among the documents of the deployment that declared it. A document declaring
another identity is refused when the deployment is created rather than skipped
when a document is judged.

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
(`modules/conformance/source/conformance/operator-protocol.hpp:434`, `:589`) and
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

**One file in a project directory is byte-exact without being JCS, and the
sentence above had not accounted for it.** The RuntimeArtifact's own
`runtime-artifact.manifest.json` is hashed and compared — its sha256 *is* the
artifact root hash — so the same rule applies to it, and what it must be exact
in is the artifact reader's fixed spelling rather than RFC 8785: `parseManifest`
walks literal separators and refuses a BOM, whitespace, or any trailing byte
(`modules/task/source/task/runtime-model-file.cpp:340-376`). Nobody writes it by
hand either; it is emitted, and its digest is what the deployment names.

*And a repository that normalizes text does not own a project directory.*
`scripts/fix_format.py` requires every `.json` here to end in exactly one
newline, which is exactly the trailing byte the installer refuses — so an
example artifact root could not carry its manifest at all while the normalizer
reached it. Ruled 2026-08-11: the normalizer must not own files whose bytes a
digest pins, and it is implemented by the marker rather than by a path — a
project directory is exactly a directory holding `umbraflow-project.json` at its
root (§2.1), and `fix_format.py` excludes such a directory and everything under
it. A second project directory added later is covered without editing the
script. What left the normalizer's count are 40 files; four of them are pinned
by nothing (each project's two root manifests) and lose only house style, and
the other 36 are schemas, plugins, manifests and a page model whose bytes some
digest already names.

**Member order is unspecified everywhere a human writes, and that follows from
the same rule.** Ordering matters only where bytes are hashed and compared, so
it matters in the registration and nowhere else, and the loader produces the
registration's order itself by handing a value to `json::canonicalBytes`. The
one sequence an author could get wrong — `project_artifact_roots`, which
`validateClaims` requires in JCS order — is sorted by the loader out of
`artifact_blobs` (§2.2). Nothing else in a project directory has an order any
reader depends on.

### 2.6 The format is schema-validated, by the framework's own schemas

Yes, and by the same evaluator that validates project payloads. There are five
of them: `umbraflow-project/v1` and `umbraflow-conformance/v1` for the two root
documents, and the three §2.4 names for the deployment's own documents.

**They live in `modules/deployment` as exact bytes, not as files in `schema/`,
and that is a correction of this section rather than a shortcut.** Three of the
five already landed there at `8277fe3`, beside the seven operator protocol
schemas the module carries; writing them out again under `schema/` would produce
a second copy that no code reads and that nothing holds to the enforcing one.
`schema/*.json` is the published spec-bundle surface — globbed by
`tests/test-runtime-surface.py:501`, counted by `tests/json/test-schema.cpp:546`
— and it is not an input to anything at run time. A framework schema that
judges a project belongs where the judging happens.

A sixth framework schema starts binding without being written, and it is the one
exception to the paragraph above because it was already published:
`schema/umbraflow-project-registration-v1.schema.json` spells exactly the
members of `ProjectRegistrationClaims`. Because Q3 made the registration a
document the loader derives, its shape is the framework's, and that file is what
`manifest_schema_hash` names (`manifest.hpp:26`, `:102`). The loader carries its
exact bytes, and `tests/deployment/test-project-directory.cpp` reads the file and
requires the two to be byte-identical — so `manifest_schema_hash` is the digest
of the published document, and a reformatting of that file is red rather than a
silently moved identity for every project in existence.
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

There is no "absent means the default" reading anywhere. Nine rules, each a
refusal that names what it read. Eight and a half are the loader's; the half
R8 keeps outside it is Q2's bill, and R8 says why:

- **R1.** Each load requires the root documents it reads, at their fixed names.
  Absent → refusal naming the absolute path and the document.

  **Amended 2026-08-12: "both, always" was wrong, and one loader enforcing it
  was the defect.** `umbraflow-project.json` is required of every project;
  `umbraflow-conformance.json` is required only by
  `deployment::loadConformanceProject`. Measured on the consuming project: the
  single loader demanded of every directory both documents, an `under_test` and
  a `foreign` role played by two *different* deployments, and per role a
  `mutating_tool`, a distinct `other_mutating_tool` and an
  `approval_required_plan_tool` each carried as `mutating` — so a directory the
  framework would open had to publish at least four mutating tools across two
  deployments, and a project at a read-only phase that honestly implements
  nothing mutating could not be expressed. The comment above the conformance
  schema said the separation already existed ("Production never opens it") while
  the production path demonstrably opened it. There are now two entry points:
  `loadProductionProject` (this document, the RuntimeArtifact, every
  deployment's five authorities) and `loadConformanceProject` (that load, plus
  the conformance document, the probe frame, the two roles and R8's vocabulary
  agreements). The second is the first plus a layer, not a second reader.
- **R2.** Every member of every framework-owned document is `required`, and every
  object sets `additionalProperties: false`. A missing member and an unknown
  member are both refusals. No member has a default.

  **With one exception, which the rule as first written silently forbade: every
  object also admits an optional `$comment` of type string, and no reader reads
  it.** A closed object rule with no exception leaves a project no place to say
  why its own document is shaped as it is, and all four documents uf-chaos
  already carries have such a comment — one of them recording why `chaos.click`
  is absent from the catalog, which is the entire reason `absent_tool` has a
  value. A repository whose rule is that a comment states a constraint should
  not hand its consumers a format in which no constraint can be stated. It is
  `$comment` rather than a member of the framework's own choosing because JSON
  Schema already spells it that way and the evaluator already accepts it as an
  inert keyword, so a project author writes one word in two places rather than
  two words. Nothing derives meaning from it and nothing may: it is bytes inside
  a document whose digest a registration pins, which is exactly as much
  authority as a comment should have.
- **R3.** Every path member is a manifest spelling in `ConfinedRoot`'s sense
  (`confined-file.hpp:52-55`): forward slashes, no empty, `.` or `..` component.
  Checked before anything is opened.
- **R4.** Every named file must exist and be a regular file inside the confined
  root. Missing → refusal naming both the manifest member and the path. This is
  the directory form of `REQUIRE(found != k_schemaFiles.end())`
  (`contract/provider.cpp:311`), and it is only equivalent if the loader errors
  on a missing file rather than skipping it.
- **R5.** Every stated `sha256` must equal the digest of the bytes it names:
  the journal manifest's per-schema digests, the reconcile manifest's
  `reconcile_schema_sha256`, and the catalog's `tool_precondition_sha256`
  (§2.4). The registration states none, so there is nothing about it to check
  here — its digests are the loader's own arithmetic. Disagreement → refusal
  printing the stated digest and what the deployment carries. This is the only
  rule of the nine that a mistyped digest can make go red, which is the whole
  of what R5 is now for. None of the three digests sits beside a path, so Q3's
  reason for removing hand-typed digests does not reach them; see §7.0.

  **The diagnosis half of that sentence was a promise two of the three sites
  did not keep**, which matters because Q3's ruling leaned on it —
  "fixing it is a copy and no tool is needed" is only true when there is
  something printed to copy. Measured on 2026-08-11: the journal site printed
  the stated digest and never what the deployment carries; the catalog and
  reconcile sites printed neither, refusing with a bare literal. All three now
  print both sides, and `tests/deployment` asserts both at each site — the
  catalog site had no case at all until then, so R5 was one rule with two of
  its three sites covered.
- **R6.** Every schema must compile under the evaluator's closed keyword set.
  A keyword the evaluator does not implement is a refusal, not a skip
  (`json-schema.hpp:9-16`, enforced today by
  `REQUIRE_MESSAGE(schema.has_value(), path)` at `contract/provider.cpp:330`).
- **R7.** Both `mutability` and `surface` are required on every catalog row; a
  row omitting one is refused rather than read as `Mutating`/`Privileged`.
- **R8. Every agreement whose two halves are both authored in this directory is
  checked here, where they were written, rather than where a suite trips over
  them.** That criterion is the rule; the list is what currently meets it, and
  a new pair of authored halves joins the list rather than being argued about:
    - `baseline_entry.event_type` equals the deployment's registered
      `baseline_event_type` (`suite-support.cpp:162-165`).
    - `mutating_tool`, `other_mutating_tool` and `approval_required_plan_tool`
      name tools the deployment's Tool Catalog carries as `Mutating`;
      `read_only_tool` one it carries as `ReadOnly`; `absent_tool` one it does
      not carry; and `other_mutating_tool` differs from `mutating_tool` (§2.3).
    - The four journal entry payloads differ from one another (§2.3).
    - Every file in `journal_payload_schemas` is named by a journal manifest
      entry, and every entry names one of those files (§2.4).

  The one half that cannot be here is the probe frame's decoded extent, which
  must be the model's (`requireProbeGeometry`, `observation-fixture.hpp:233-254`,
  called at `suite-support.cpp:171`) — after Q2 the model's extent does not
  exist until the Host has activated the artifact, which a loader does not do.
  That check stays in the conformance run and moves *later* rather than earlier,
  to just after `activateObservationHost` (`observation-fixture.hpp:540-554`),
  where the binding first exists. It is the one half with no case in
  `tests/deployment/`, and it is what Q2 cost: a directory can no longer be
  refused for a probe frame its own model does not fit until something installs
  and activates the artifact.

  Six of the seven bullets above were accepted by the loader until 2026-08-11,
  every one of them measured against the real uf-chaos directory rather than
  reasoned about. What they had in common is that each is a claim one document
  makes about another in the same directory, which is why the criterion is
  stated first and the list second.
- **R9.** A named file must be the kind of file its member names it as, where
  the member names one. There is exactly one such member: `probe_frame` names a
  capture, so its bytes must decode as a PNG (`image::decodePng`). The decoded
  pixels are discarded — what a Host is later handed is the project's own bytes,
  and a second copy in another encoding would be a second spelling of the frame.
  This is not R8's second half in disguise and does not weaken it: "it decodes"
  and "its extent is the model's" are different claims, and only the first is
  answerable before an artifact is activated.

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
real cost inside a product binary, and it is not confined to the suite's cases:
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
set once by the run before `context.run()` and never written after — declared in
`modules/conformance`, not in a public header, and read through a function rather
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
`docs/archive/reviews/2026-08-10-third-round-review.md:493-499` recorded that number
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
`modules/deployment/source/deployment/project-deployment.{hpp,cpp}`. Not a file
of their own: `8277fe3` had already put the envelope schemas there, so there was
nothing left for an `operator-envelope.{hpp,cpp}` to hold.**
`requireExactMembers` (`contract/provider.cpp:549-577`), `validateReduceInput`
(`:595-647`), `validateDeriveInput` (`:649-679`), `validatePlanInput`
(`:681-723`), `validateStepInput` (`:725-744`) and the null passthrough
`validateNullableDocument` (`:579-593`) judge documents `ledger.cpp` assembles
itself; there is no schema file for them anywhere and no project knowledge in the
question. `readPlanProposal` and `readStepIntent` are the same class of thing
and went to the same place at `b6c7e72`, out of `operator-protocol.hpp:434` and
`:589` and into `uf::deployment`, with the suite including them from there
rather than owning them. This closes consumer-onboarding's Q1 by making it
moot: nobody writes an envelope reader because nobody writes a provider.

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
  its public dependency list on what a consumer compiles against. **Landed
  2026-08-12**, one directory deeper than written: the fold is into
  `modules/conformance/source/conformance/`, which is where a module's headers
  live, and the manifest — now `modules/conformance/manifest.txt` — rests its
  public list on what the module's own headers name.
- `ProjectRuntimeArtifact` (`provider.hpp:51-55`) and `ArtifactFile` (`:31-35`),
  and with them `artifactManifestRow` (`observation-fixture.hpp:126-134`) and
  `publishRuntimeArtifact` (`:139-178`) — 60 lines that re-serialize a manifest
  the project already published.
- `ProvidedProject::lastReduceInput` and `::lastDeriveInput` (`:196-202`).
- `cmake/conformance-suite.cmake`, all 277 lines.
- `conformance/exemplars/umbraflow/provider.cpp` (95),
  `conformance/exemplars/arcana-expedition/provider.cpp` (1,063), and both
  `CMakeLists.txt`.
- The project-construction half of `project-fixture.hpp` — everything from
  `registrationBytes` through `makeProject` and the four owner `create` calls.
  *Not yet, and deliberately.* Step 6 moved the whole header, and both
  `project-schemas.hpp`, from `conformance/exemplars/` to
  `conformance/fixtures/`, because `tests/operator/*` and `tests/deployment/*`
  include them and Q5 defers that dependency to its own change. **They reached
  `tests/support/` on 2026-08-12**, with the suite's move into
  `modules/conformance`: `tests/support/umbraflow/project-fixture.hpp`,
  `tests/support/umbraflow/project-schemas.hpp` and
  `tests/support/arcana-expedition/project-schemas.hpp`, each include root
  chosen so no `#include` in `tests/` changed spelling. Their deletion is still
  Q5's and is what this document is still open on; where they live no longer is.

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
  `docs/archive/plans/2026-08-10-w2-effective-plan.md:1185-1189` says so in as many
  words. This module is that deployment, and the conformance binary is one of its
  callers rather than its purpose — which is the reason it is a module rather
  than something under `conformance/`.

- **Five framework schemas**, in `modules/deployment` rather than in `schema/`
  and all five landed: `umbraflow-tool-catalog/v1`,
  `umbraflow-journal-event-schema-manifest/v1` and
  `umbraflow-reconcile-manifest/v1` at `8277fe3`, `umbraflow-project/v1` and
  `umbraflow-conformance/v1` with the loader. §2.6 says why they are not files.
  A sixth, `schema/umbraflow-project-registration-v1.schema.json`, is not new —
  it is already published and starts binding rather than starts existing.
- **`entry/conformance/main.cpp`** and the `umbra-flow-conformance` target. The
  target landed in step 6; the entry point stayed at
  `conformance/source/suite-main.cpp`, on the grounds that it is one `main`
  either way and that keeping it beside the cases kept the whole switch inside
  `conformance/`, `cmake/` and two `CMakeLists.txt`. **That was scope, not
  shape, and it was corrected on 2026-08-12**: the file is
  `entry/conformance/main.cpp` as written here, the cases and fixtures are
  `modules/conformance` with `type = static`, and the two binaries now differ in
  nothing but their names. What `main` no longer holds is `runSuite`, which is
  declared by `modules/conformance/source/conformance/suite-run.hpp` — the entry
  translation unit converts `argv` and calls one function, exactly as
  `entry/cli/main.cpp` does.
- **`external/doctest`** and the `uf::doctest` INTERFACE target.
- **`cmake/conformance-run.cmake`** — landed in step 6.
- **Two in-tree exemplar directories**, `examples/umbraflow` and
  `examples/arcana-expedition`, replacing the two provider translation units and
  remaining what `conformance/CMakeLists.txt` already called them: documentation
  a consumer copies. Both are what the two CTest runs are pointed at since step
  6.
- In uf-chaos: `umbraflow-project.json`, `umbraflow-conformance.json`, two
  `plugin/*.luau` files carrying the bytes that are today C++ raw literals, two
  artifact-blob files, and two reconcile manifests. No registration document:
  Q3 put the intent in the deployment block and the digests in the loader
  (§2.2). All of that was written at uf-chaos `af3b270`.

  **And one document nobody had counted: an effect payload schema per
  deployment.** Both plugins emit an `OP:ExpectedEffect` naming
  `payload_schema_hash` `00…c1` (`plugin/dream.luau:8`), a placeholder no file
  in either tree provides — and the digest inside the effect is what selects
  which supplied schema judges it, so an effect naming bytes the deployment does
  not carry is refused (`project-deployment.hpp:104-114`).
  Until each plugin names the digest of a real schema listed in its block's
  `effect_payload_schemas`, every Plan output uf-chaos produces is refused. It
  is uf-chaos's to write and this document's to have said so.

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
`modules/task/source/task/runtime-model-file.hpp:178-221`). It lands here rather
than with the loader because it belongs to `task`, and it must land before step 6
or the loader has no fingerprint to give the suite. Unit tests only.

**3. Land the five framework schemas and the directory loader — done.** The
loader is `modules/deployment/source/deployment/project-directory.hpp`:
`loadProductionProject(directory, expected) -> Result<LoadedProject>`, which
derives each deployment's registration from its block and the digests of the
files it read and constructs all five authorities, and
`loadConformanceProject(directory, expected) -> Result<ConformanceProject>`,
which is that load plus the conformance document (split 2026-08-12; it was one
function named `loadProject` until then, see R1 in §2.7). Two of the five framework schemas were new
and three had already landed at `8277fe3` (§2.6). Its tests are
`tests/deployment/test-project-directory.cpp`, one case per rule R1–R8 except
R8's second half, each proven red by removing its rule, plus the registration
chain's teeth case (§7.0 Q3). Nothing consumes it: `provideProject` is
untouched and the suite runs exactly as before.

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

**6. Switch, in one change — done.** The suite stopped taking a C++
`ProvidedProject` and started taking a project directory. `provideProject`,
`ProjectRole` as an exported type, `ProvidedProject` with both `std::shared_ptr`
recorders, `ProjectRuntimeArtifact`, `ArtifactFile`, `ProjectProbeFrame` — and
with it the restated fingerprint — and
`conformance/include/conformance/provider.hpp` itself are gone; so are
`cmake/conformance-suite.cmake` (277 lines), both `provider.cpp` files and both
exemplar `CMakeLists.txt`. `umbra-flow-conformance` and
`cmake/conformance-run.cmake` arrived, and the two runs point at
`examples/umbraflow` and `examples/arcana-expedition`. The root
`CMakeLists.txt`'s include of the suite CMake moved inside the
`PROJECT_IS_TOP_LEVEL` guard: with no consumer `add_subdirectory` left, nothing
outside this repository reaches it. It is one change rather than two because a
tree in which some projects arrive as C++ and some as data is two spellings of
one thing, and `CLAUDE.md` forbids resting there.

`requireProbeGeometry` moved from the top of `prepareStore` into
`activateObservationHost`, just after the Host has parsed the model and before
the `ObservationRuntime` that needs the same fingerprint is built — still before
any resolution, so `requireResolvedSurface`'s account of which causes can reach
it survives the move; its wording now names `activateObservationHost` rather than
`prepareStore`, which is the only thing about it that changed.

**What the deletions cost, measured rather than reasoned.** Deleting the call and
running a project whose capture is one pixel wider than its model left the run
red — at `requireResolvedSurface`, over
`{"kind":"unknown_state","reason":"unknown_scene_competitor"}`. So
`requireProbeGeometry` is not independently falsifiable by outcome: a second
mechanism refuses the same input. What it is falsifiable by is its message, which
is the only one on that path naming both extents, and the comment at the
declaration now records that measurement instead of the `internal_error` reason
it used to predict. `docs/pitfalls/checks-that-cannot-fail.md` files that shape
under "an assertion another refusal already satisfies".

The same reordering deleted one subcase in
`tests/operator/test-state-contract.cpp`: "a fingerprint that is not the
capture's extent resolves nothing" had a caller declare a geometry, and after Q2
no caller can. The surviving half — a capture whose extent is not the model's —
reaches the resolver through `test_support::secondObservationHost`, which now
assembles its Host itself rather than through `activateObservationHost`, because
only a world a case builds can get past the refusal a project directory meets.

**Two things it did not do, and one it placed differently — all three closed on
2026-08-12.** The three headers `tests/operator` and `tests/deployment` still
include — `project-fixture.hpp` and the two `project-schemas.hpp` — moved from
`conformance/exemplars/` to `conformance/fixtures/` rather than dying, because
Q5 defers the tests' own move onto the loader to a separate change; §4.2 owed
them a home in `tests/support/`, and that is where they are. `conformance/include/`
did not fold into `conformance/source/`, for the same reason: `tests/` compiles
against it. And the entry point was `conformance/source/suite-main.cpp` rather
than §4.3's `entry/conformance/main.cpp`, which kept every file that step touched
inside `conformance/`, `cmake/` and two `CMakeLists.txt`.

The follow-up change took all three at once, and `conformance/` no longer exists:
the cases and fixtures are `modules/conformance` (`type = static`, `include/`
folded into `source/conformance/`), the entry point is
`entry/conformance/main.cpp`, and the fixtures are `tests/support/`. It moved no
count: 86 CTests with `CONFORMANCE=4 CONTRACT=40 LUAU=4 SCHEMA=19` before and
after, and 16 cases / 1,303 assertions against `examples/umbraflow`,
`examples/arcana-expedition` and uf-chaos alike.

**The evidence, re-derived rather than cited.** It was assembled in the working
tree and not committed: both registrations exist at once only there, because
`conformance/fixtures`' `makeProject` is the C++ side and
`deployment::loadProject` over `examples/umbraflow` is the data side. `plugin_id`
(`fixture.alpha`), `baseline_event_type` (`fixture.baseline`) and the artifact
roots (both empty) are identical. All eight digests differ, and the
`project_registration_hash` with them, for three reasons demonstrated rather than
asserted:

- **A file carries a trailing newline that a C++ raw literal does not.** Each of
  the four schema documents is byte-for-byte the literal plus one newline; the
  first differing byte is the last one.
- **A digest inside a document moves with the document it names.** Removing the
  trailing newline from each of the three manifests and mapping the file digests
  back to the literal digests reproduces the C++ bytes exactly, for the tool
  catalog, the journal manifest and the reconcile manifest. The plugin is the
  same: substituting one embedded `payload_schema_hash` turns the C++ bytes into
  the file's, byte for byte.
- **`manifest_schema_hash` stops being a placeholder.** The C++ fixture hashed
  the string `"registration-schema"`. The loader hashes
  `schema/umbraflow-project-registration-v1.schema.json`, whose bytes are
  identical to what `projectRegistrationSchemaBytes()` carries. That is Q3's
  ruling landing: the framework's already-shipped registration schema starts
  binding.

Nothing that agreed when this comparison was first produced at `a57bf05`
disagrees now. One expectation in the earlier draft of this step was wrong:
`tool_catalog_hash` was to differ "by exactly the members Q6 dropped", and it
does not — **because Q6 no longer says to drop them.** §2.4 records that its
first half was overruled when the loader landed and its second answered
differently: `plugin_id` stays and is checked against the deployment's, since a
catalog that answered for whichever registration presented it would be a catalog
no registration owns, and the path `tool_precondition_schema` was replaced by the
digest `tool_precondition_sha256`, which is the only route by which the
precondition schema's bytes reach `tool_catalog_hash`. Both members in
`examples/umbraflow/schema/alpha/tool-catalog-v1.json` and uf-chaos's
`schema/dream/umbraflow-tool-catalog-v1.json` are therefore required rather than
owed for deletion, and the framework schema does force them. **Nothing here is
outstanding.**

Two constraints this step had to satisfy or the gate would have gone red for
unrelated reasons, both met. `tests/CMakeLists.txt` requires the CTest names
`contract-control-c01`, `-c06`, `-c09`, `-c10`, `-c11`, `-c12` and `-c13`;
`uf_add_conformance_run` registers the same seven, beside the same aggregate
`conformance-umbraflow`, and configure fails if a claimed case and a declared
`TEST_CASE` disagree in either direction — measured in both directions. And the
suite CMake's include moved inside the guard, as above.

**7. Delete uf-chaos's `contract/`, its root `CMakeLists.txt` and its generated
headers — the precondition is met.** `umbra-flow-conformance --project
E:/umbraflow-projects/uf-chaos` ran green on 2026-08-11, 16 cases and 1,303
assertions, the same figures both in-tree examples produce; that repository has
no C++ of its own left to verify it any other way. Not earlier, and specifically
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
read the recorders —
`modules/conformance/source/conformance/suite-control-ledger.cpp:498` ("the
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
`modules/conformance/source/conformance/suite-control-ledger.cpp:23, 49, 83,
125, 155, 247, 349` and
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

**The RuntimeArtifact is kept and is verified more strictly.** `runtime-model.toml`,
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

  *Settled in step 3.* The loader has exactly one `verifyExact` call site, and
  it is always the empty case: it passes the digest of the bytes it is passing
  in, and says so. Branching there — the recorded hash when a caller had one,
  the computed hash otherwise — would have written the same comparison twice
  and made the empty case hard to see. So the recorded hash is compared once,
  one line above, in the loader's own refusal, which is the only place that can
  name both values.
- **The one check with teeth must be proven to have them.** Against a stored
  session naming hash `H`, flip one byte of a pinned file in the project
  directory, reload, and resume: the refusal must fire and must print both
  hashes. If that does not go red, there is no real check anywhere on this
  chain, and the whole ruling is unfounded. It is a work item, not a hope.

  *Done in step 3, and it goes red.* The case is
  `tests/deployment/test-project-directory.cpp`, "a project whose bytes moved
  under a stored session is refused". It records a `SessionManifest`'s
  `project_registration_hash`, flips one byte of the plugin file, reloads, and
  requires the refusal to carry both hex digests. The byte is in the plugin
  deliberately: those bytes reach `project_registration_hash` through
  `plugin_hash` and through nothing else, so no other rule can refuse the
  flipped directory — and the case reloads it a third time with no commitment
  at all and requires that load to *succeed*, which is what proves the refusal
  came from this check rather than from a second mechanism. Removing the
  comparison leaves the case green, measured.

  One thing the work item asked for was not in the framework and was not this
  step's to add: no refusal in `modules/operator` printed either hash, so the
  chain refused a moved project in three places and only the loader's said what
  had moved. *Closed 2026-08-11 by `4ae4fcf`.* `ProjectRegistration::verifyExact`
  (`manifest.cpp:304-315`) and `pinSession`'s registration-agreement refusal
  (`ledger.cpp:2815-2826`) each print expected and computed. The third
  (`ledger.cpp:2892-2912`) is an existence query rather than a comparison —
  `project_instances` is keyed by `(plugin_id, project_instance_key)` and
  `SessionPin` carries no `plugin_id` — so it prints the compound key it searched
  for, and says at the refusal why there is no second value to name.

The review's one stated uncertainty, recorded rather than resolved: during a
first load, with no stored session, a mid-load modification is undetectable. Its
judgement, which this ruling accepts, is that nothing is owed there — no prior
commitment exists to violate — and that aligning a first load with an external
expectation is the signature design's job, not this one's.

**Q5 — yes, after step 6, as its own change.** As recommended.

**Q6 — ruled "drop the restated members", and overruled by the loader landing.**
The ruling was: nothing is released, and under Q3 the loader recomputes every
hash regardless, so uf-chaos's recorded values move once rather than twice. What
holds now is §2.4, which is where the members are described. `plugin_id` stays in
the catalog and in the journal manifest and is checked against the deployment's,
because a catalog that answered for whichever registration presented it would be
a catalog no registration owns — a property of the document rather than a copy of
the block. The path `tool_precondition_schema` was answered differently rather
than dropped: it became the digest `tool_precondition_sha256`, which restates
nothing and is the only route by which the precondition schema's bytes reach
`tool_catalog_hash`. Both members are required in
`examples/*/schema/*/tool-catalog-v1.json` and in uf-chaos's own catalogs, and
the framework schema forces them.

**Q7 — refuse a directory with one deployment.** As recommended: a skipped case
is a green result promising more than it verified.

**Q8 — keep `--frames`.** It is not new machinery. It is the same loader pointed
at a directory of captures instead of at one probe frame, and it is the only
thing that drives a model over real ones. Dropping it would delete a real check,
and would require the project's host to keep a C++ build alive in order to keep
it — the opposite of what this document is for.

**Q9 — one function in the suite's own sources.** As recommended; it is
`setProjectDirectory` in `modules/conformance` since 2026-08-12, and `runSuite`
rather than `main` is what calls it.

**Q10 — recorded, not ruled, and it blocks nothing.** A recognition gap is a
framework work item with the project's measurement attached, never a C++ hatch
in a project.

**Reconciled, 2026-08-11, against `319bdb1`.** §0, §1.1, §1.3, §2.2–§2.7, §3,
§4.1–§4.3, §5 and §6 were amended in place so that no section still describes
the shape a ruling replaced. What moved: the registration document became the
deployment block plus a document the loader derives (Q3); the conformance
document now says in as many words that it carries no `fingerprint`, and where
the geometry comes from instead (Q2); the three project manifests kept
`plugin_id` and traded the path `tool_precondition_schema` for the digest
`tool_precondition_sha256`, which is Q6 overruled rather than applied — §2.4 and
the Q6 ruling both say so; §4.1's two
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

**Reconciled again, 2026-08-12, against the suite's move into
`modules/conformance`.** §4.2's two open items and §4.3's entry-point placement
landed and say so above; the Q6 ruling was rewritten, because it still read
"drop the restated members" while §2.4 and §5 step 6 both record that the loader
landing overruled it. Citations to files that moved rather than died were
repointed at `modules/conformance/source/conformance/`; citations to
`conformance/include/conformance/provider.hpp` were left alone, because that
file was deleted and a path is the only thing that can still name it.

Seven things the reconciliation, and the work since it, turned up — recorded
rather than edited around.

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
digest written beside the file it describes is a second spelling — appeared to
apply verbatim to the journal manifest's per-schema `sha256` and the reconcile
manifest's schema digest. Applying it there would put the 13 journal payload
schemas and `reconcile-v1.schema.json` outside every hash in the design, because
those digests are the only path by which their bytes reach
`project_registration_hash`. §2.4 states the narrow rule that keeps them and
§2.7 R5 is what is left of the rule that checks them.

*Resolved rather than excepted, 2026-08-11, while step 3 was implemented.* The
tension was in the reading, not in the design. After `8277fe3` none of the three
surviving digests sits beside a path: the journal manifest names an event type
and a digest, the reconcile manifest names two definitions and a digest, the
catalog names a digest and no schema at all, and every path in the directory
lives in the deployment block (§2.2). So a digest and the path of the file it
describes are never written side by side anywhere in the format, and Q3's reason
is satisfied throughout rather than suspended for two documents. What a project
author types is a digest of bytes they own, in the one place where that digest
is what the document is *for* — which is why R5 remains the one rule a mistyped
digest can redden, and why it is not an exception to anything.

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
name means" (`modules/task/source/task/runtime-model-file.hpp:159-167`) — is
unaffected, because the fingerprint is a sibling of `DeclaredRuntimeUi` on the
binding rather than a fourth vocabulary inside it.

**The one refusal the suite adds of its own had no case at all.** Moving the two
readers left `planAuthority` wrapping the step reader in a check that a
`UIActionIntent` names the run's agreed `uiAction`, and nothing downstream
repeats it — `task::DispatchAuthority` carries no UI identifier and the ledger
stores the intent bytes without reading them
(`conformance/include/conformance/provider.hpp:144-149`). Replacing that check
with `static_cast<void>(uiAction);` left `conformance-umbraflow`,
`conformance-arcana`, `test-operator` and `test-contract-operator` green to the
assertion, because no case ever made the two sides disagree. The case that now
does is in `modules/conformance/source/conformance/suite-project-authority.cpp`,
one row per
identifier, and it disagrees on the *run's* side: a plugin's bytes are the
project's and are pinned by `plugin_hash`, so a mismatched intent cannot be
obtained from a plugin at all, and a step minted under the foreign project's
plugin is refused for the registration it names long before any step is read.

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
surface into `DeclaredRuntimeUi` (`runtime-model-file.hpp:158-172`). **The
counter-argument is real:** `DeclaredRuntimeUi`'s comment says a reader "may ask
whether a name is in one of them, which is identity, and cannot ask what the name
means", and `tests/test-runtime-surface.py` enforces that the Operator reads no
RuntimeModel semantics. Whether a base resolution is identity or semantics is the
ruling. **Blocks:** whether `umbraflow-conformance.json` carries a
`fingerprint` member, and a small reordering in `prepareStore` (the fingerprint
would be available only after the Host activates the artifact, so
`requireProbeGeometry` moves after `activateObservationHost`). *Both executed in
step 6; the document carries no `fingerprint`, and the reordering put the check
inside `activateObservationHost` rather than after it.*

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
`docs/archive/plans/2026-08-11-consumer-onboarding.md:886-902` already identified as
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
- [A project is a directory of data](2026-08-11-project-as-data.md) — the
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
