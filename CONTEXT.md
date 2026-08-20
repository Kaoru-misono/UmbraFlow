# UmbraFlow

A reusable game-automation host: C++ owns target, capture, input and evidence
safety; trusted Luau resolves a screenshot-free RuntimeModel; Operator holds the
session, lease and journal authority; a game connects only through a data-only
project plugin.

This file is the terminology authority — a rename lands here first, then
propagates. Every current entry names the constant, function, type or schema
`$def` that pins it. A claim with nothing to point at does not belong here.
Verified against the tree on 2026-08-11.

Design authority for anything this file does not settle:
[runtime hardening rewrite](docs/plans/2026-08-09-runtime-hardening-rewrite.md)
for design and the consumer repository's
`docs/architecture/parallel-implementation-plan.md` for current requirement
state. *(Amended 2026-08-12: the archived next-block
record also named the W2-W7
reconciliation as the tie-breaker "where same-day specifications conflict".
There are no live same-day specifications left to break ties between — all four
landed and all five documents are in
[`docs/archive/plans/`](docs/archive/plans/) — so the reconciliation is a record
of rulings, not a third authority. What it and they still owe is now carried in
the consumer repository's parallel implementation plan.)*

## Reading a spelling

A concept is often spelled three ways — C++ type, Luau field, wire key — and the
differences are load-bearing, not sloppiness. Where they differ, all three are
given. Two further traps:

- A file name is not an id. `umbraflow-trace-v2.schema.json` holds
  `umbraflow-trace/v2`. Read the `const` or the constant, never the listing.
- PascalCase in prose is not proof of a type. `ControlledTarget`, `UiTarget` and
  `OperatorSession` are real concepts with no C++ type of that name.

## Current vocabulary

**Controlled target** — one attached window/process the Operator holds a lease
over. **`controlled_target_id` is the only spelling**, and it is one word in
every layer: the wire (`$defs.OperatorSession`, `$defs.ControlLease` in
`schema/umbraflow-operator-v1.schema.json`), the DDL columns, and the C++
(`ControlLease::controlledTargetId` in
`modules/operator/source/operator/ledger.hpp`). There is no `ControlledTarget`
type; the word survives only in two ledger error messages. Window-instance
freshness is the separate value `TargetGeneration`
(`modules/domain/source/domain/ids.hpp`).

_Avoid_: `controlled_target_key`, `controlledTargetKey` (the C++ and DDL
spelling until 2026-08-11 `07abc3e`, which renamed 14 files and left zero
residual sites). The two spellings were bridged by a literal rename inside the
ledger, `.controlledTargetId = controlledTargetKey`; that bridge was deleted
rather than relocated. The rename moved the Operator DDL fingerprint to
the value then recorded by `k_operatorDatabaseSchemaIdentity` in
`modules/operator/source/operator/ledger.cpp`, over the same 23 tables, so an
`operator-runtime.sqlite` from before it is refused at open and deleted, never
migrated.

> **Corrected 2026-08-12: the fingerprint above is one break out of date, and it
> is not the only column rename this file was missing.** The current Operator
> DDL fingerprint was the value then recorded by
> `k_operatorDatabaseSchemaIdentity` in
> `modules/operator/source/operator/ledger.cpp`, over the same 23 tables. It was
> the **sole** Operator schema identity as of 2026-08-12; `user_version` carries
> nothing and is not written, and a database whose identity differs is refused
> and left untouched rather than deleted. The ruling and what it obliges a
> migration author to name
> are in [the archived execution checklist](docs/archive/plans/2026-08-19-upstream-execution-checklist.md), under the delete-on-open
> deadline. It moved because
> `journal_events` and `project_state` — both journal records — were made to
> carry their schema definitions' member names. **These four are now the one
> spelling for their columns**, and each old spelling is an `_Avoid_`:
> `opaque_project_payload` (was `canonical_event`, which named bytes that are
> not an event), `provenance` (was `canonical_provenance`),
> `project_state_schema_hash` (was `state_schema_hash`) and
> `canonical_opaque_payload` (was `canonical_state`). Deciding artifact:
> [journal record binding](docs/archive/plans/2026-08-11-journal-record-binding.md),
> whose §9 asked for exactly this edit and had gone unexecuted; it is archived
> as of this correction.

> **Re-settled 2026-08-13 by U8.** The current Operator DDL fingerprint is
> the value then recorded by `k_operatorDatabaseSchemaIdentity` in
> `modules/operator/source/operator/ledger.cpp`, still over the same 23 tables
> and still the sole identity. Three columns moved
> with it: `sessions.controller_capabilities` is new and carries the exact JCS
> array `capability_profile_hash` is now derived from, `operation_plans` gained
> `policy_hash` and turned `required_approvals` from a 0/1 integer into the JCS
> array of approver capabilities the pinned PolicyArtifact ruled, and
> `approvals.approver_capability_hash` became `approver_capability` so an
> approval can be matched against that array. Each old spelling is gone rather
> than accepted beside the new one, and a database at the previous identity is
> refused at open as before.

> **Re-settled 2026-08-13 by U2b.** The current Operator DDL fingerprint is
> `k_operatorDatabaseSchemaIdentity` in
> `modules/operator/source/operator/ledger.cpp`, over 24 tables, and remains the
> sole identity. U2b added the Operator-private
> `observed_instance_bindings` table and its immutability/cleanup-refusal triggers.
> The table stores the canonical authority and opaque ID bidirectionally and has no scope
> ownership or cascading deletion: cleanup remains impossible until a future
> owner can prove every Journal, Operation, backup and audit reference expired
> or was deleted. Scope closure alone therefore cannot remove a binding.

**RuntimeArtifact** — a verified manifest, one `runtime-model.toml`, and the
manifest-listed assets under `assets/`. Pinned as
`k_runtimeArtifactManifestFileName`, `k_runtimeModelFileName` and
`k_runtimeAssetDirectoryName` in `modules/task/source/task/runtime-model-file.hpp`.
Never an annotation screenshot bundle.

**RuntimeModel** — the schema-owned tree inside `runtime-model.toml`. Its nouns are
`ui_target`, `locator`, `reader`, `binding`, `surface` and `transition`, and its
top level also carries `schema_version`, `base_resolution` and `base_dpi`.
`$defs.runtime_model` in `schema/umbraflow-runtime-v3.schema.json`; built,
checked and frozen by `model.compile` in `modules/task/runtime/model.luau`.

**UiTarget** — semantic identity alone: no pixels, no placement, no page. Spelled
`ui_target` in the schema and in Luau; no C++ or Luau type.

**Binding** — the sole owner of one actionable placement, keyed by surface,
UiTarget and variant. Its record is exactly
`{id, surface, ui_target, variant, placement, detector, actions}`
(`$defs.binding`; `bindingBuilder` in `model.luau`). `placement` is
`{kind = "fixed", rect, action_point?}`, and `action_point` is present exactly
when some action is a `click` — a Binding granting only keystrokes carries none,
because a keystroke names no coordinate. Enforced on three sides, in
`model.luau`, in `tools/annotate/model_file.py`, and by the schema's `allOf`.
An action is `{id, kind, proof_locator}` plus `key` when `kind` is `"key"`;
which key names exist is `uf::KeyName`'s and nowhere else's.
`RuntimeModelBinding`
(`modules/task/source/task/runtime-model-file.hpp`) is a different thing: the
host's handle to one loaded model, not one row of it.

**Variant** — the named visual form of one binding. `$defs.identifier` under
`$defs.binding`, `$defs.resolved_binding` and `$defs.receipt_request`; built at
`model.luau`'s `bindingBuilder`. It is the live word.

**Surface** — one screen state: `{id, kind, covers, identity}` with `kind` one of
`scene | overlay | interrupt` (`$defs.surface`). A scene covers nothing; an
overlay or interrupt must name a lower surface it covers, which is what makes
stacking a declared fact rather than runtime bookkeeping. Stack validity is
`model.valid_surface_stack` / `model.surface_covers`.

**Transition** — `{id, from_surfaces, trigger, to_surfaces}`, where the trigger
is `{binding, action}` (`$defs.transition`). Destinations are a set. Where a
transition leads is a falsifiable fact and lives in the model; whether to take it
is policy and does not.

**State resolution / binding resolution** — the two same-cycle decisions.
`resolution.resolve_state` and `resolution.resolve_binding` in
`modules/task/runtime/resolution.luau`; documents `$defs.state_resolution` and
`$defs.binding_resolution`, each a `oneOf` over resolved / unknown / ambiguous.
Neither is a type in any language. `StateResolution` survives as a word only in
comments and in `SnapshotRecord::stateResolutionHash`.

**Receipt** — the host-minted, opaque, one-shot authority to deliver one input:
a click at the point the model measured, or one keystroke, which names no point.
Requested through `cycle:authorize` (`modules/task/runtime/observe.luau`),
transferred exactly once by `resolution.take_receipt_request`, and minted by the
`runtime_receipt` native in `modules/task/source/task/ffi/uf-tables.cpp`. At most
one per observation cycle: `receipt_requested` is set before the mint and never
cleared. Its request shape is `$defs.receipt_request`, and the proof it rests on
is the binding's own `proof_locator`, re-measured on this cycle inside
`resolution.authorize`.
_Avoid_: page token, page proof, resolve result (token is reserved for
`std::stop_token`; the other two lose that a Receipt is host-minted, not a value
the framework can compute).

**ProjectPlugin** — the only game extension boundary, data in and data out.
`ProjectPluginHandle`, `ProjectPluginRegistrar` and `ProjectPluginFunction`
(`Derive | Plan | NextStep | Reconcile | Reduce`) in
`modules/operator/source/operator/project-plugin.hpp`; the pinned registration is
`VerifiedProjectRegistration` in `modules/operator/source/operator/manifest.hpp`.
The production registration generation still supplies one Luau source file as
a one-module closure. The script kernel beneath it already accepts a pinned
multi-module closure plus a separately pinned execution environment, with
host-owned closed `require`; it does not concatenate sources. Project-directory,
registration and release readers do not expose that kernel yet, so the
replacement is not production-readable until the atomic module/resource cut in
[`2026-08-20-project-luau-module-vfs-capabilities.md`](docs/plans/2026-08-20-project-luau-module-vfs-capabilities.md)
lands.

**Operator protocol** — the session, lease, snapshot, operation and journal
vocabulary. It exists **only** as JSON `$defs` in
`schema/umbraflow-operator-v1.schema.json` — `OperatorSession`, `ControlLease`,
`SessionManifest`, `Operation`, `ToolInvocation`, `ToolResult`,
`ReconcileProposal` and the rest. Some have C++ counterparts under a different
name (`ControlLease` and `SessionManifest` do); `OperatorSession` has none, and
`frameworkProjectGlobals()` in
`modules/task/source/task/framework-bundle.cpp` keeps business execution closed
until it does.

**Conformance suite** — the exported Operator gate a consuming repository runs
against its own project directory. One word in every position, ruled 2026-08-11:
tree `conformance/`, include prefix `<conformance/...>`, namespace
`uf::operator_runtime::conformance`, shipped binary `umbra-flow-conformance`,
CMake function `uf_add_conformance_run`, CTest name `conformance-<project>`,
CTest label `CONFORMANCE`. The label shares no substring with the per-case labels
`CONTRACT` and `SCHEMA` and must not: `ctest -L` is a regex, and the previous
`CONTRACT-SUITE` made `-L CONTRACT` report 44 gates where 40 exist. A consumer
compiles nothing and calls no CMake function of ours: it runs the binary this
repository ships, as `umbra-flow-conformance --project <directory>`.
`uf_add_conformance_run` is how this repository registers its own two runs, and
lives inside the `PROJECT_IS_TOP_LEVEL` guard for that reason;
`examples/umbraflow` and `examples/arcana-expedition` are the two directories it
names, each written the way a consumer writes its own.
_Avoid_: contract suite, `contract-suite`, `contract-suite-<project>`,
`operator-contract/`, `uf_add_operator_contract_suite`, label `CONTRACT-SUITE`,
`contract-suite/fixtures/` (all retired 2026-08-11), and
`uf_add_conformance_suite`, `conformance/exemplars/`, "exemplar" for a project
(retired later the same day, when a project became a directory). Keep
`contract-<area>-<id>` and `schema-<area>-<id>`: those name individual gates, not
the suite.

**LoadedProject** — everything one project's directory hands the product,
constructed by `deployment::loadProductionProject(directory, expected)` in
`modules/deployment/source/deployment/project-directory.hpp`. A project is a
directory of data with no C++ of its own: `umbraflow-project.json` names every
other file, and the loader derives each deployment's registration from that
deployment's block and the digests of the files it read, then builds all five
authorities from them. One deployment is enough and no tool has to be mutating.

**plugin_authoring** — the deployment-block member that says which of the two
authoring paths wrote the behavior `plugin` names: `generated` for an adapter
the project kit produced from an `umbraflow-declarative-workflow-tool/v1`
declaration, `hand-written` for author-owned ProjectPlugin behavior. The current
directory generation represents that behavior as one Luau module; the accepted
next generation represents it as an entry plus a closed module set. The author
states the tier because only the author knows it: neither source bytes nor a path
convention proves how the behavior was authored, and a rule the kit could apply
but the runtime loader could not would make the two readers drift apart.

**plugin_justification** — the deployment-block member stating which member or
semantic of `umbraflow-declarative-workflow-tool/v1` cannot express this
hand-written plugin. Required of a deployment whose `plugin_authoring` is
`hand-written` and refused from one whose `plugin_authoring` is `generated`: the
declarative tier is the default and hand-written behavior is the exception,
whether that behavior occupies one module or a module closure; demanding a
reason from the default is demanding a false one.
Both `project check` and `loadProductionProject` refuse an absent or blank one,
and neither judges whether the stated reason is true — that stays a review
obligation at plugin acceptance
([checks that cannot fail](docs/pitfalls/checks-that-cannot-fail.md)). Blank
means ASCII whitespace: a justification of `U+00A0` alone is accepted, and is a
review finding on the same terms as one that is present and false.
_Avoid_: a version of this rule that reads the `plugin` path to decide which
tier a deployment is on (retired 2026-08-14, and it had the rule inverted:
it demanded a justification from every deployment with a `plugin` member,
generated adapters included).

**umbraflow-project.json's shape** — stated once, in
`schema/umbraflow-project-v1.schema.json`, and reaching both of its readers as
published bytes through the framework schema catalog. The two readers are
`deployment::loadProductionProject` and the offline kit's `project build` /
`project check`, and they live in modules that cannot link one another —
`uf::deployment` reaches `uf::task`, while the `project` executable links
`uf::project` and `uf::core` alone. The kit reads the document from the source
root whether or not the author declared it as an input, and a source tree
holding none is not a project.

This is the implemented generation. The accepted module/resource cut replaces
it with `umbraflow-project/v2` and registration format 3 in one release; no
reader accepts a half-migrated single-file/module-closure hybrid.

_Avoid_: `k_projectSchema` inside
`modules/deployment/source/deployment/project-directory.cpp`, and
`validatePluginJustifications` inside
`modules/project/source/project/project-kit.cpp` (both retired 2026-08-14; the
second was a weaker partial copy of the first that accepted an empty
`deployments` array, an empty `plugin` path, a numeric deployment `name` and any
unknown member).

**Template cuts** — a project declares the Locator templates its build cuts in
`template_cuts` at the root of `umbraflow-project.json`, stated even when
empty. Each names a `template` path under `generated/templates/`, its
`source_sha256s`, and the `rect` to cut. A source is named by content and never
by path, because it is a capture of a running target rather than a file of the
project — that is what lets a repository hold Locator templates without
referencing a screenshot anywhere. `readProjectManifest` in
`modules/project/source/project/project-kit.cpp` reads the member out of the
document the published schema has just accepted, which is the same parse and
therefore not a second reading of it.

Resolving a hash to bytes stays the caller's job and never becomes the kit's:
`TemplateSourceResolver` takes a `ContentHash` and returns bytes, so
`uf::project` never learns what a directory is. `project build`, `project check`
and `project freeze` are that caller and take `--frames-root PATH`, a
hash-named store where the bytes of `H` are the file `H.png`. It is a flag and
not an environment variable because nothing in this repository reads the
environment and a build whose output depends on ambient state has no record of
what produced it. A machine with no corpus still gets a resolver: a declared cut
that cannot be resolved is refused by name — `generatedTemplates` puts the hash
in the message rather than the error's context, because the `project` command
line prints `message()` and nothing else — and is never silently skipped.
Resolved bytes are re-hashed and refused on mismatch in that one place, so a
resolver is not trusted to verify itself.
_Avoid_: `ProjectTemplateCutSpec` as a member of `ProjectBuildSpec` (retired
2026-08-15; a caller-assembled cut was a second spelling of a declaration the
document already owned, and the command line, which assembled none, could not
reach the capability at all).

**ConformanceProject** — a `LoadedProject` plus the second root document,
`umbraflow-conformance.json`, constructed by
`deployment::loadConformanceProject`. It carries the production load in
`loaded`, the probe frame, and the two roles a run takes, `underTest` and
`foreign`, each a `ProjectConformanceRole` naming one deployment and the
vocabulary that drives it — `foreign` is deliberately not under test, and
`underTest` keeps the phrase because it is honest about which of the two
registrations a run observes. The split is 2026-08-12's
([the loading question](docs/archive/plans/2026-08-11-project-as-data.md) §2.7 R1):
before it one loader required both documents, two roles and four mutating tools
of every directory, so a consuming project at a read-only phase could not be
expressed at all.
_Avoid_: `deployment::loadProject` (one entry point requiring both documents,
split 2026-08-12); `ProvidedProject`, `provideProject`, `ProjectRole` as an
exported type, `provider.cpp`, `<conformance/provider.hpp>` (all retired
2026-08-11 when a project became a directory); `ProjectUnderTest`,
`projectUnderTest`, `project-under-test.hpp` (retired earlier the same day).
`task::UiActionUnderTest` is unrelated and current: it is the action a run
drives.

**Executable specification resolution** — one of the four places
[the hardening rewrite](docs/plans/2026-08-09-runtime-hardening-rewrite.md)
picks one side of a contradiction inside the frozen v1.9 bundle and freezes that
choice upstream. Renamed from "executable conformance resolution" on 2026-08-11,
when `conformance` was given to the suite: the wider audience wins the word, and
these are resolutions of a specification conflict, which the new name says.
_Avoid_: executable conformance resolution.

## Schema ids

**One id travels in band and is re-read.** `umbraflow-trace/v2`:
`serializeTraceEvent` writes it as the `schema` field of every event
(`modules/trace/source/trace/event.cpp`), it is `k_traceSchema` in
`event.hpp`, and `schema/umbraflow-trace-v2.schema.json` pins it as a `const`.

**The RuntimeModel id does not travel.** `umbraflow-runtime/v3` occurs exactly
twice in the tree, as `model.schema` and `project.schema` in
`modules/task/runtime/{model,project}.luau`, and nothing reads either back. What
actually travels in `runtime-model.toml` and is validated is the integer
`schema_version`, required to be `3` by `model.luau` and by
`$defs.runtime_model` alike.

`umbraflow-external/v1` names an external-payload manifest, pinned as
`SUPPORTED_SCHEMA` in `scripts/fetch_external.py` and written as `schema` in
`modules/{ocr,operator}/external/manifest.toml`.

Every other file in `schema/` is identified by its `$id` alone and carries no
in-band id. The `$id`s are not uniform, so read the file rather than guessing:
`journal-v1`, `operator-v1`, `policy-v1`, `project-registration-v1` and
`trace-v2` are short ids under `https://umbraflow.local/schema/`, while
`umbraflow-annotation-workspace-v2.schema.json`,
`umbraflow-runtime-artifact-v1.schema.json` and
`umbraflow-runtime-v3.schema.json` spell the full file name under
`https://umbraflow.dev/schema/`.

**No schema digest is pinned outside its schema file.** Editing any document
under `schema/` moves no constant, refuses no recorded manifest and reddens no
gate; `tests/test-runtime-surface.py` therefore carries no rule keeping such a
copy synchronized. What crosses a boundary instead is a **contract
generation** — a small integer a producer declares and an acceptor compares:

| Manifest | Members | Acceptor |
|---|---|---|
| release manifest | `annotation_workspace_format`, `workspace_sqlite_revision` | `k_annotationWorkspaceFormat`, `k_workspaceSqliteRevision` in `modules/operator/source/operator/runtime-installation.hpp` |
| RuntimeArtifact manifest | `runtime_artifact_format`, `runtime_model_format` | `k_runtimeArtifactFormat`, `k_runtimeModelFormat` in `modules/task/source/task/runtime-model-file.hpp` |

`modules/task/runtime/model.luau` states `model.format`, the RuntimeModel
generation its trusted parser reads. `TaskHost::finalizeRuntimeModel` refuses an
artifact whose parser answers with a different number, so a `model.format` out
of step with `k_runtimeModelFormat` reddens every activation rather than
refusing artifacts in production.

The file digests that verify a RuntimeArtifact's closure — `page_model.sha256`,
`assets[].sha256` and the artifact root hash — are untouched by any of this and
still refuse a single mutated byte.

_Avoid_: `annotation_workspace_schema_hash`, `workspace_sqlite_schema_hash`,
`manifest_schema_hash` on a **RuntimeArtifact** manifest,
`runtime_model_schema_hash`, `model.schema_hash`,
`k_annotationWorkspaceSchemaHash`, `k_workspaceSqliteSchemaHash`,
`k_runtimeArtifactSchemaHash`, `k_runtimeModelSchemaHash` and the Python
`SCHEMA_ROOT_HASH` (the spellings until 2026-08-15; each made a cosmetic edit to
a schema file, or a comment inside the authoring workspace DDL, refuse every
artifact already published against it). ProjectRegistration's own
`manifest_schema_hash` is a different field and stays: it is derived from the
build-generated catalog and refused at
`modules/operator/source/operator/manifest.cpp`.

## Scripting

**Capability namespace (`uf`)** — the intended read-only global root through
which a project task script would reach the host. **Nothing builds it today**:
`scriptHostTableInstaller()` in `modules/task/source/task/ffi/uf-tables.cpp`
returns a no-op installer, and `scriptProjectGlobals()` returns `{}`, so no `uf`
table is registered and none is published. `uf` remains the reserved spelling,
the same product abbreviation as the C++ `uf::` namespace, used deliberately in
both languages.
_Avoid_: `uf.recognizers` (this table's spelling until 2026-07-31; the capability
model had already made it false, because the table held exactly the elements a
script may CLICK and excluded the identify-only ones that do the recognising),
`umbra` (the 2026-07-27 spelling of this root, renamed to `uf` on 2026-07-29 —
the rename touched only this script root, never the product names `UmbraFlow` and
`umbra-flow` or any schema id), `bot` (superseded draft wording).

**Project globals** — what a project script may name. Five lists reach a project
environment and they differ: `k_projectStandardGlobals`
(`modules/script/source/script/ffi/environment.cpp`) is the deterministic
standard-library floor every environment gets; `scriptProjectGlobals()` returns
`{}`, so no host table is published; and of the three framework whitelists in
`modules/task/source/task/framework-bundle.cpp`, `frameworkProjectGlobals()`
returns `{}` on purpose so business execution publishes nothing,
`explorationProjectGlobals()` returns `{"explore"}`, and
`runtimeProjectGlobals()` returns `{"jcs", "observe", "project"}` for the trusted
runtime VM `TaskHost::bootTrustedRuntime` boots. The first two framework lists
are asserted in `tests/task/test-framework-bundle.cpp`, which also proves `ctx`,
`explore`, `model`, `observe`, `project`, `navigation`, `input` and `receipt` are
all nil in a business VM. All five, and every boot site that assigns one, are
read by `published_global_errors` in `tests/test-runtime-surface.py`.

**Private capability surface** — the host-built table of primitives only trusted
Luau can reach. What makes it private is that it has no name in either
environment: the host builds it, `installSandbox`
(`modules/script/source/script/ffi/sandbox.cpp`) hands it to the framework bundle
as its chunk argument, then drops its own reference, so the only way to reach it
afterwards is a closure it was handed to. There are two of them and neither key
set is guessable:
- exploration, `buildAnnotationSurface` — `explore_cycle_open`,
  `explore_cycle_close`, `explore_crop`, `explore_probe`, `explore_project_read`,
  `explore_project_write`, `explore_terminal`, plus the one non-capability field
  `error_tag` carrying `"uf.error"`.
- trusted runtime, `RuntimeNativeState::install` — `runtime_model_bytes`,
  `runtime_semantic_hash`, `runtime_model_finalize`, `runtime_asset`,
  `runtime_cycle_open`, `runtime_match`, `runtime_read`, `runtime_receipt`,
  `runtime_cycle_close`.
Both are deep-frozen at the end of the build. **Neither carries a click, a key
press or any other input primitive**, and `math.random`/`math.randomseed` are
nilled outright by `installSandbox` rather than offered here.
_Avoid_: native driver, private native surface, raw verbs, "never a key of any
table" (a 2026-07-29 draft wording, replaced: the primitives are exactly the keys
of the private table — read literally the old phrasing said the code violates its
own design).

**Framework environment** — the writable globals table the trusted Luau
framework's modules load under, and the only environment whose metatable chains
`__index` to the VM's main globals. Built by `installFrameworkEnvironment`
(`modules/script/source/script/ffi/environment.cpp`) and labelled
`uf.framework_env`. Environment isolation in Luau is **per closure, not per
thread** — `luau_load` takes the env a chunk closes over, and a new thread copies
its parent's globals — so this is a real table in the VM registry, never the
`luaL_sandboxthread` proxy shape, which is the `_G` escape the design exists to
exclude.
_Avoid_: framework sandbox, trusted thread, framework `_G`

**Project environment** — the globals table one project task run executes under:
an explicit whitelist with **no metatable at all**, so there is no `__index`
chain to the framework environment or the main globals.
`installProjectEnvironmentPrototype` registers it frozen under
`uf.script.project_env`, and `pushProjectEnvironment` shallow-copies it per run
and leaves the copy writable, so globals a run writes die with that copy. The
absence of the metatable is the isolation property itself; the denial list is a
consequence of it, not the mechanism.
_Avoid_: script sandbox, task environment, per-run `_G`

**Tier B error carrier** — the value a recoverable automation failure is raised
as: a userdata the host mints under its own Luau tag, wearing a protected
metatable labelled `uf.error` (`k_errorType`, decoded by `decodeTierB`, both in
`modules/task/source/task/ffi/uf-tables.cpp`). C++ decides what it is by the tag
alone; the label is what tells it apart from the other host userdata a script can
hold. A project script can mint no tagged userdata at all — `setmetatable` and
`table.clone` take tables, and `newproxy` is nilled by `installSandbox` — so the
carrier cannot be forged however exactly a table copies its fields.
_Avoid_: error table (the shape before 2026-07-29 `c37ee5b`, where identity
rested on a metatable a `table.clone` could carry along), exception, error object

**Task** — one automation flow authored as a Luau script and executed by the host
in its own isolated VM against one target window. A task always belongs to
exactly one project and is addressed by its name within that project, never by a
loose file path.
_Avoid_: script (the source artifact a task is written in, not the unit of
execution), job, macro

**Project** — everything the host needs to automate one game target: the
authoring workspace, source screenshots, the generated RuntimeArtifact, and the
tasks written against them.
_Avoid_: workspace, profile

## Runtime

**Engine session (`engine::EngineSession`)** — the stateful engine capability
scope for one bound target. It **owns** its frame source, action sink and
optional OCR engine (three `std::unique_ptr` members) and **borrows** its trace
recorder: `m_recorder` is a `trace::TraceRecorder&`, so that a run has one
evidence stream and one sequence counter. The header states the resulting
lifetime contract — the composition root must construct the recorder first,
destroy it after, and never relocate it while the session borrows it. The name
describes the lifetime of that capability bundle; it does not imply the object
has a `CaptureSessionId`.
_Avoid_: engine run (conflates the capability scope with execution identity and
`EngineRunId`), capture session, "owns a trace sink" (it borrows one)

**Capture session identity (`CaptureSessionId`)** — the outer component of frame
identity. With `TargetGeneration` and `FrameId` (all three in
`modules/domain/source/domain/ids.hpp`) it prevents evidence from distinct
capture sessions from colliding. Supplied by the composition root; it belongs to
captured frames, not to `engine::EngineSession`.
_Avoid_: `SessionId` (too generic), engine session id, task session id

**Observation cycle** — the explicit open/close scope around exactly one capture,
inside which state resolution, binding resolution and a single authorized input
all read the same frame. A keyboard sequence therefore costs one cycle per
keystroke, which is the design and not a limit to work around. `CycleLedger`
(`modules/task/source/task/cycle-ledger.hpp`) holds **at most one** open cycle,
which is why "the proof and the action came from one frame" is not a check that
can be forgotten but a state that cannot be expressed. Opening costs one capture;
closing releases the frame deterministically and is idempotent. The Luau side is
`observe.open` returning a cycle object with `resolve_state`, `resolve_binding`,
`authorize` and `close`.
_Avoid_: frame box, observation lease (the freshness contract on a delivered
action, not this scope), capture scope

**Ticket (`task::CycleTicket`)** — all a script ever holds of an open cycle:
`{generation, ordinal}`, both re-checked on every use. `generation` names the
ledger that minted it and `ordinal` the cycle within it, so a ticket left over
from a spent VM generation is rejected rather than colliding with a live ordinal
in the next one; the stamp comes from `mintHandleGeneration()`. The frame never
crosses into Luau, so the moment its several megabytes are released is a host
decision rather than the Lua collector's.
_Avoid_: frame handle, cycle object, token (reserved for `std::stop_token`)

**Trace stream validator (`trace::TraceStreamValidator`)** — the state machine
one run's evidence stream must pass, in
`modules/trace/source/trace/stream-validator.hpp`. `admit()` checks exactly four
things and nothing else: the stream begins at sequence 1, and thereafter
`session_id`, `session_manifest_hash` and `producer` are stable and the sequence
advances by exactly one. A rejected event does not advance validator state.
`TraceRecorder` owns one and runs it on every event before the stamp
(`recorder.cpp`), and the recorder is the only path to a sink in the codebase, so
it cannot be gone around.
_Avoid_: trace schema validator (the schema is the wire format; this validates
the sequence), emit guard, event sanitizer (nothing is sanitized — a refused
event is rejected outright, never truncated)

**Trace event wire shape** — fixed by `serializeTraceEvent` in
`modules/trace/source/trace/event.cpp` and by
`schema/umbraflow-trace-v2.schema.json`. The whole key set is `schema`,
`event_type`, `session_id`, `session_manifest_hash`, `monotonic_sequence`,
`recorded_at_unix_millis`, `audit` (`actor`, `producer`, `references`) and
`payload` (`schema_hash`, `fields`). Payload fields are scalars only —
`TraceScalar` admits no bytes, arrays or nested objects — which keeps production
trace a small semantic stream rather than a covert replay bundle.

**Delivery events** — which line a delivery is written under is decided in
`EngineSession` and nowhere else. A click emits `engine.action_delivered`
unconditionally (`modules/engine/source/engine/session.cpp`); the other verbs
emit `engine.key_delivered`, `engine.scroll_delivered`,
`engine.long_press_delivered`, `engine.drag_delivered` and
`engine.pointer_move_delivered`. Cropping emits nothing from the engine: the
honest line is `annotation.region_saved`, written by
`modules/task/source/task/task-context.cpp`, which is the layer that also knows
the encoded bytes and their hash.

## Exploration

**Exploration environment (`explore`)** — the privileged authoring-only Luau
surface, `modules/task/runtime/explore.luau`, published as the single project
global of an `ExplorationSession` and never of a business VM. Its top-level verbs
are `explore.cycle`, `explore.probe`, `explore.read`, `explore.write` and
`explore.terminal`. Pixels come out through `view:crop(...)`, a method on the
frozen view object `explore.cycle` passes to its callback — **not** a top-level
`explore.crop`. `explore.cycle` clears its own `live` flag before closing, so a
view kept past the callback refuses rather than cropping a frame that is gone.

It has **no click and no input verb of any kind**: the exploration private
surface carries none, so an agent cannot name a bare coordinate today. When one
is reintroduced it belongs here rather than on a business surface, because a bare
click has no binding and no surface behind it and the vocabulary has to stay
honest.
_Avoid_: handing a task raw pixels or a bare click, "operator mode" (the `drive`
front-end was a separate consumer with no model access at all; it was retired
into this one on 2026-08-03 in `eafc273`, so the phrase now names nothing)

**CLI verbs** — the product dispatches exactly two, `explore` and `targets`
(`entry/cli/main.cpp`). `check`, `replay` and `run` are retired and enforced as
such by `RETIRED_COMMANDS` and `ALLOWED_COMMANDS` in
`tests/test-runtime-surface.py`.

## Retired vocabulary

This section is the file's memory. A word here was real once; it is listed so
that meeting it in an old plan, an old commit or a proposal costs a lookup rather
than a re-derivation. Nothing here may define a current API, schema or type.

**Enforced retirements.** `tests/test-runtime-surface.py` refuses these, and it
runs under `ctest -L CI` as `check-repository-surface`, so they cannot creep
back:
`ContextDetector`, `ContextResolution`, `ContextTruth`, `RuntimeState`, `UFR`,
`context_detector`, `context_resolution`, `context_truth`, `.ufr`,
`find_element`, `mint_hit`, `resolve_page` (`RETIRED_RUNTIME_SYMBOL_PATTERN`);
declaring a type named `Element`, `Hit`, `Page` or `UFR`
(`RETIRED_TYPE_DECLARATION_PATTERN`); the `check`/`replay`/`run` CLI verbs and
their argument and dispatch symbols (`RETIRED_CLI_*`); and every name in
`FORBIDDEN_PROJECT_GLOBALS` — `ctx`, `click`, `model` and `receipt` among them —
as a project global in any of the five lists that publish one, except that
`explore`, `observe` and `project` are each allowed in the one list that
publishes them today (`PUBLISHED_GLOBAL_AUTHORITIES`). The rule reads a binding
and not a table's members, so `key`, `drag` and `scroll` remain legal as methods
of the cycle view `explore` hands out.

**Retired schema ids.** `umbraflow-authoring/v4`, `umbraflow-annotations/v3`,
every earlier spelling of both, and the constants that carried them,
`k_authoringDocumentSchema` and `k_runtimeManifestSchema` — all went out with the
C++ model in `a80ea07` (2026-08-01) and appear in no file under `schema/`,
`modules/`, `entry/`, `tools/` or `tests/`. `umbraflow-project/l2-v2` was
replaced by `umbraflow-runtime/v2` in `8af22bc` (2026-08-09). No old id has a
read path; an unknown schema string fails with the ordinary unsupported-schema
error.

**Retired annotation-model nouns** (the pre-2026-08-01 C++ and Luau model). None
of these is a type anywhere in the tree:
- `Element` / `model.Element`, and its own retired spellings `recognizer`,
  `region`, `annotation` (the three-way kind), `RecognizerId`, `ElementId`. The
  RuntimeModel splits what `Element` conflated into `ui_target` (identity),
  `locator`/`reader` (evidence) and `binding` (placement and actions).
- `CompiledElement`, `CompiledAppearance`, `RuntimeElementSpec`,
  `RecognizerDefinition` — the per-element compiler, retired 2026-08-01: the
  layer-two model IS the runtime form, and a template is now a PNG blob named by
  an `$defs.asset_path` that `TemplateStore::load` turns into a
  `TemplateTicket` (`modules/task/source/task/template-store.hpp`).
- `ElementCapabilities`, `ExercisedCapabilities`, `isSubsetOf`, `Capabilities`,
  `Holding`, `exercised`, and the older `AnnotationType`, `ElementKind`,
  `AnchorElement`, `InteractiveElement`, `InfoElement`,
  `PageAnchor`/`ActionTarget`/`InfoRegion`. Authorisation is now the binding's
  `actions` list and its `detector`.
- `model.Reference` and its field set `{pageId, elementId, holding, exercised,
  rect_override?, appearance?}` — the real `binding` record shares no field with
  it. Also retired with it: `searchRoi` as a model key, `allowed_page_ids` /
  `allowedPageIds`, `bool shared`, `PageSignature::create`, `placement` as the
  name of that row. Note `searchRoi` is still the live spelling of an unrelated
  C++ parameter in engine, vision and task.
- **`Appearance` / `model.Appearance` / `appearance`** — renamed to `variant`.
  This direction has been recorded backwards before: `variant` is the live word
  (three uses in `model.luau`, six in
  `schema/umbraflow-runtime-v3.schema.json`) and `appearance` occurs nowhere in
  the runtime model or its schema. `std::variant` and the UUID variant field keep
  their own names and are unrelated.
- `navigation.Edge`, `navigation.stack_new`, `walk_edge`, `rect_override`, and
  the runtime page stack. The successors are `$defs.surface` with its
  `kind`/`covers` pair and `$defs.transition`; overlay depth is a declared
  covering relation checked by `model.valid_surface_stack`, not a stack a run
  keeps. The design constraints that outlived the spelling: no `go(any page)`
  pathfinding in the host, and an overlay is never modelled as an ordinary
  transition.
- `oracle`, `regress`, `cycle_match`, `oracle.Expectation`, the falsification
  matrix and the `umbra-flow check` / `umbra-authoring check` verbs that walked
  it. `modules/task/runtime/` holds exactly `evidence`, `explore`, `jcs`,
  `model`, `observe`, `project`, `resolution`. The rules worth re-deriving if
  measurement returns: never record measurements back into the file as
  expectations, never read an undeclared page as "the same page as the other
  one", and never require that a screen resolve NO other surface — an overlay
  legitimately resolves its base.

**Retired script and trace surface.** `scribe` and `scribe.luau`; `FrontEnd` and
so `FrontEnd::Annotation`; `script-validator.hpp`; `observe.resolve_page`,
`observe.click`, `observe.long_press`, `observe.walk_edge`,
`evidence.mint_receipt`; `uf.pages` and `uf.elements`; the open-step-path
concept with its `steps` wire array, `retry_attempt` field and step-id / step-key
spellings; and the trace keys `seq` and `elementId`. `observe.luau` exports
`observe.open` and nothing else, and
`tests/task/test-receipt-request-v2.luau` asserts `observe.click == nil`.
`annotation.click_delivered` is named in one `session.hpp` comment as the line an
exploratory click would take and is emitted by nothing.

**Retired `core` facilities** (all four removed 2026-08-11). Listed with the
reason, so the same proposal meets the evaluation instead of repeating it:
- `Synchronized<T>` (`core/concurrency/synchronized.hpp`) — never had a caller.
  It offered only scoped access through a callback and could not express
  waiting, while the tree's single real mutex, `FrameSlot` in
  `modules/controller/source/controller/platform/windows-capture.cpp`,
  deliberately pairs its `std::mutex` with a `condition_variable_any` so a waiter
  can honour a `stop_token`. Its one unique contract — a synchronized operation
  may not return a pointer or reference to protected storage — was rescued into
  `cpp-coding`'s safety profile as a forward-looking rule.
- `ControlFlow<V>` / `Continue` / `Break` (`core/control/control-flow.hpp`) — its
  one plausible call site was rejected on the merits, not for want of callers.
  `modules/vision/source/vision/sad.hpp` declares `SadSearchControl` (the poll
  vocabulary: `Continue`, `Cancelled`, `TimedOut`) and `SadSearchStopReason` (the
  report vocabulary, which adds `ComparisonBudgetExhausted`) at deliberately
  different widths. Collapsing them would let a poll callback mint
  `ComparisonBudgetExhausted`, a value only the matcher may produce, making an
  unrepresentable invalid state representable.
- `Flags` (`core/types/flags.hpp`) — no candidate; there is not one bitmask-valued
  enum in first-party code.
- `NonZero` (`core/types/non-zero.hpp`) — no candidate was found.

**Retired framing.** Context truth, direct `ctx` action methods, direct
run/check/replay input, caller-supplied coordinates, caller-supplied effects, and
compatibility readers are not implementation targets. There is no compatibility
path anywhere in this repository, by the standing rule in `CLAUDE.md`: a shape
that must change is changed outright and its callers with it.

## History

Everything below is a record of what was decided on a date. Read it for what a
word once meant and why it moved — never as a description of the tree.

> **2026-07-31 (capabilities).** The two annotation schema ids read
> `umbraflow-authoring/v2` and `umbraflow-annotations/v1` until the capability
> model landed. Both were bumped in one atomic change, and neither old id had a
> read path. Deciding artifact:
> `docs/archive/plans/2026-07-31-annotation-model-capabilities.md` §三 — itself superseded
> on 2026-08-11 in `8be0b35`.

> **2026-07-31 (vocabulary rename).** All three ids moved again, because a key
> rename is a wire change. `umbraflow-authoring/v3` → `v4` (`[[annotation]]` →
> `[[element]]`, `[[variant]]` → `[[appearance]]`, a reference's `variant` →
> `appearance`, `recognizer_kind` → `element_kind`), `umbraflow-annotations/v2` →
> `v3` (`[[recognizer]]` → `[[element]]`, plus the same two appearance keys), and
> `umbraflow-trace/v1` → `v2` (`recognizerId` → `elementId`, the resources line's
> `recognizers` array → `elements`). `[[annotation]]` was the oldest instance of
> the same defect: it spelled the anchor/target/info taxonomy the capability model
> had already retired, so a persisted key named a classification that no longer
> existed. It moved inside the bump that was happening anyway rather than costing
> a v5.
>
> Of the three lines this note tracks, only `umbraflow-trace/v2` still exists, and
> the `variant` → `appearance` half was reversed by the Runtime v2 rewrite.

> **2026-08-01 (`a80ea07`).** The C++ annotation model was deleted. Element, page,
> reference, appearance and edge became trusted-Luau values, and the compiler that
> emitted one runtime artifact per element went with it.

> **2026-08-02.** `rect` stopped being required on an element. It had only ever
> meant WHERE TO LOOK, and some elements have no answer of their own — a minimap
> cell is matched at coordinates worked out per frame because the map pans, and one
> confirm button drawn once sits somewhere different on every screen that shows it.
> The rule that came out of it survives in `$defs.fixed_placement`: exactly one
> party supplies a rectangle for any one use, and "absent" means "the caller says
> where", never "nobody said".

> **2026-08-03 (`eafc273`).** The `drive` front-end was retired into the agent one.

> **2026-08-09 (`8af22bc`).** `umbraflow-project/l2-v2` was replaced by
> `umbraflow-runtime/v2`, and Page/Element/Hit/UFR, Context truth, direct task
> input and compatibility APIs ceased to be implementation targets. Authority:
> [runtime hardening rewrite](docs/plans/2026-08-09-runtime-hardening-rewrite.md).

Older design notes whose *reasoning* still holds, though their rosters do not:
`docs/archive/plans/2026-07-29-three-layer-task-system.md` §4 (why the observation cycle
and the ticket are shaped this way), §5 (the four invariants of the private
capability surface — its twelve-primitive roster is the 2026-07-29 draft, not
today's two tables) and §12 (why the trace validator exists at all — its event
grammar predates the current key set).
