# 2026-08-23 — Project state interpretation removed

For a project author holding a declaration written against an earlier release.
This note says what **broke**, what each removed member **was**, and what — if
anything — replaced it. The reasoning is in
[the ruling](../decisions/2026-08-23-the-framework-stops-interpreting-project-state.md);
the normative shape is `schema/umbraflow-project-v3.schema.json` and the
published surface is [`docs/PUBLIC-CONTRACT.md`](../PUBLIC-CONTRACT.md).

> **Historical upgrade step.** The `child_effects` instructions below describe
> this release boundary only. The 2026-08-26 flat-call ruling subsequently
> deleted that member, Tool bodies and nested Project-handler calls. Do not add
> them to a current declaration; use the generated public contract for the
> current shape.

## Your declaration is refused, not degraded

The `schema` const is now `umbraflow-project/v3`. A document carrying
`umbraflow-project/v2` — or anything older — is refused at `project build` and
`project check`:

```text
umbraflow-project.json: schema/umbraflow-project-v3.schema.json refused schema: const is not the required value
```

There is no fallback reader, no version flag, and no absent-means-the-old-thing
reading anywhere in this change. Nothing degrades to a partial load. Migrate the
document in one edit.

## A deployment now carries exactly nine members

`name`, `plugin_id`, `plugin_authoring`, `plugin_justification` (optional),
`tool_closure`, `tool_bindings`, `tools`,
`observed_instance_identity_schemas`, `resources`, and an optional `$comment`.
`additionalProperties` is `false`, so anything else is refused by name.

The document's top level is unchanged apart from the `schema` const.

## What was removed, and what replaced it

| Removed member | What it was | What replaces it |
| --- | --- | --- |
| `plugin` | the deployment's module graph, `{entry, modules}` | **`tool_closure`** — the same graph plus a mandatory `exported_entry_points` |
| `reducer_closure` | a second, pure closure exporting `reduce` | nothing; there is one closure because there is one program |
| `tool_catalog` | a path to a `umbraflow-tool-catalog/v1` document | **the inline `tools` array** — the entries themselves, no file |
| `tool_precondition_schema` | a path to a document whose `$defs` held every Tool's argument shape | nothing; argument shape moved into each Tool's own `argument_schema` |
| `project_state_schema` | a path to the schema of the state the framework folded | nothing |
| `baseline_event_type` | the event type the genesis Journal entry carried | nothing |
| `journal_event_schema_manifest` | a path to the per-event-type payload schema manifest | nothing |
| `journal_payload_schemas` | paths, `minItems: 1`, one per Journal payload | nothing |
| `effect_payload_schemas` | paths to the typed payload schemas of proposed effects | nothing; an effect's payload is pinned by digest and never parsed |
| `project_observation_schema` | a path to the observation document's shape (removed in an earlier release) | nothing |
| `reconcile_schema`, `reconcile_manifest` | the reconcile SPI's document shapes (removed in an earlier release) | nothing |
| `observed_instance_identity_schemas` | an array of **file paths** | an array of **`{name, schema}`** with the schema inline |

`resources` is unchanged and still names files. A resource is opaque supply the
framework delivers without reading, not declarative material.

Inside a Tool entry, these are gone: `tool_precondition_sha256`,
`effect_payload_sha256s`, and `result_schema`. So are the old catalog document's
own wrapper members, `schema` and `plugin_id` — there is no catalog document to
wrap anything.

## `plugin` became `tool_closure`

This is the transition a diff cannot show you, because the member was renamed
and gained a member at the same time. If your declaration still has a `plugin`
block, more than one shape change stands between it and v3:

- An earlier release replaced `plugin` with **two** closures, `tool_closure`
  and `reducer_closure`. Its notes described the new capability and never said
  the old member had been deleted or that an existing declaration would be
  refused, so a project still on the `plugin` shape hit a wall those notes had
  not warned about. That is the mistake this note exists to avoid repeating.
- This release deletes `reducer_closure` again. One closure remains.

`tool_closure` is `{entry, exported_entry_points, modules}`. `modules` and
`entry` are what `plugin` held. `exported_entry_points` is new and mandatory:
you **state** what the entry module exports, and the loader compares that
statement against `tool_bindings` and against the bytes. It is not derived from
either, because a table compared with itself proves nothing.

## `tool_catalog` became the inline `tools` array

Take the `tools` array out of your `umbraflow-tool-catalog/v1` document, drop
the document's `schema` and `plugin_id` wrapper along with
`tool_precondition_sha256` and `effect_payload_sha256s`, and paste the array in
as the deployment's `tools`. Then delete the file.

`tool_catalog_hash` is now the SHA-256 of the canonical (RFC 8785 JCS) bytes of
that array. Widening any bound moves it, and therefore moves
`project_registration_hash`.

## The reducer contract and durable project state are gone entirely

Deleted, not deprecated: the Journal event schemas, the reducer, the project
state revision, and the final compare-and-swap. Nothing folds a project's state.
There is no revision to read and none to write.

Anything in your Luau that existed to fold state must be rewritten or dropped —
including the module that exported `reduce`, and any code deriving a value from
folded state. If a fact your code reads came out of project state, it currently
has no source: the published contract has no project-owned durable store. A
Project-owned, opaque, byte-exact channel is the intended replacement and its
contract is frozen in the ruling, but it is not implemented and is not in the
public contract. Design around its absence today rather than around its promise.

The on-disk migration ran in the same change: `journal_events`, `project_state`
and the proposal tables are dropped, and `project_instances`,
`project_observations` and snapshots are rebuilt without their ProjectState
columns. Nothing is left for you to migrate.

## Argument validation is now the project's choice

Every Tool entry carries a mandatory `argument_schema` whose value is one of two
things:

- an inline JSON Schema object — the framework enforces it;
- the exact string `"unchecked"` — you are declining argument validation.

The key is always present. There is no absent-means-unchecked reading, and the
two values mean different things rather than being two spellings of one. When
you decline, the framework still records the exact bytes it passed, their digest
and their call coordinates, so traceability is undiminished.

Cross-Tool `$defs` indirection is gone with `tool_precondition_schema`. Reuse
inside one schema is JSON Schema's own `$defs`; two Tools repeating the same
definition verbatim is accepted and correct.

## A hello-world project now authors no JSON Schema at all

One `umbraflow-project.json` and one Luau source. That is the whole floor. A
project that binds Tools and declares `"unchecked"` writes no JSON Schema
either. You author schema only when you ask the framework to enforce something:
an argument shape, or a semantic identity basis.

A project that binds no Tool still writes `tools`, `tool_bindings`,
`observed_instance_identity_schemas`, `resources` and `template_cuts` as empty
arrays, and its `tool_closure` still states an empty `exported_entry_points`.
Each is required to be present precisely so that abstaining is stated rather
than looking like a declaration that went missing.

## Two rules `project check` will not catch

Neither is new in this release, but both are checked only when a deployment is
**loaded**, and `project build` and `project check` do not load the closure.

- **Every Tool name must sit under the deployment's `plugin_id`.** With
  `plugin_id` `chaos.dream`, a Tool named `chaos.get_current_event` is refused —
  `Tool name chaos.get_current_event is outside the namespace chaos.dream its
  registrant owns` — and must be `chaos.dream.get_current_event`.
- **`child_effects` is mandatory on every Tool entry**, with all five members
  present. A Tool that issues no child call writes the empty declaration: no
  names, zero calls, and the most restricted ceiling of each kind. A Tool whose
  entry point calls another Tool — including a framework Tool such as
  `framework.screen.observe` — must name it in `child_tool_names` and admit at
  least one call, or the call is refused however permissive every other bound is.

Run `umbra-flow open --project DIR` after `project check`. That is what compiles
the closure, compares its exports against `tool_bindings`, and applies both
rules above.

## Also moved in this release

- The registration schema is `https://umbraflow.local/schema/project-registration-v4`.
  It lost `reducer_closure`, `project_state_schema_hash`,
  `project_tool_precondition_schema_hash`, `journal_event_schema_manifest_hash`
  and `baseline_event_type`.
- The conformance wire tag is `umbraflow-conformance/v3`.
- The wire tag `umbraflow-project-declared-files/v1` is gone. `project build` no
  longer records declared schema files by digest, because a deployment no longer
  declares any.
- `contract_versions` in a release manifest names `umbraflow-project/v3`. The
  binaries in an `umbraflow-bin/` installed before this release compile the
  older schema and refuse a v3 document, so that bundle must be replaced rather
  than reused; a suite that drives it cannot verify a migrated project.

## Migrating, in order

1. Change `schema` to `umbraflow-project/v3`.
2. Rename `plugin` to `tool_closure` and add `exported_entry_points`, listing
   exactly the entry names your `tool_bindings` bind. Delete `reducer_closure`.
3. Inline the catalog's `tools` array into the deployment. Delete the catalog
   file.
4. Give every Tool entry an `argument_schema`: inline the definition its old
   `argument_schema` string named, or write `"unchecked"`. Delete
   `tool_precondition_sha256`, `effect_payload_sha256s` and `result_schema` from
   each entry. Make sure `child_effects` is present and names every child Tool
   the entry calls.
5. Namespace every Tool name under `plugin_id`.
6. Replace each `observed_instance_identity_schemas` path with
   `{name, schema}`, the name being the identity your observation proposals
   carry as `identity_schema_id`.
7. Delete `baseline_event_type`, `project_state_schema`,
   `project_observation_schema`, `tool_precondition_schema`, `reconcile_schema`,
   `reconcile_manifest`, `journal_event_schema_manifest`,
   `journal_payload_schemas` and `effect_payload_schemas`, and the schema files
   they named.
8. Rewrite or drop every Luau module that folded, read or wrote project state.
9. Run `project build`, `project check`, and `umbra-flow open --project DIR`.
   There is no input ledger to refresh: the file set is derived from the
   declaration on every run. See
   [the upgrade path release note](2026-08-23-project-upgrade-path.md).
