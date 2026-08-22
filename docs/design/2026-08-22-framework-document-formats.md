# Framework document formats

Three of the documents a deployment ships are the **framework's** formats rather
than the project's: the Tool Catalog, the journal event schema manifest, and the
reconcile payload schema manifest. A project authors the bytes, but the shape,
the vocabulary and the refusals are this repository's.

The normative authority is the schema bytes embedded in
[`modules/deployment/source/deployment/project-deployment.cpp`](../../modules/deployment/source/deployment/project-deployment.cpp),
compiled and applied by `deployment::validateFrameworkFormat`. Which schema
judges a document is that document's own `schema` member, so a document naming a
format this repository does not own is refused rather than judged by whichever
schema came first. No prose here restates a field those bytes own.

## Why the worked examples live here

`tests/deployment/test-project-directory.cpp` reads the three blocks below by
their `<!-- example: ... -->` anchors and requires `validateFrameworkFormat` to
accept each one. That test is what keeps a worked example a document the
framework accepts instead of prose beside a C++ string constant — the
arrangement that once had a consumer writing CamelCase for `mutating` and
`semantic`.

That pin is also why this document exists. The examples were first stated in
[a project is a directory of data](../archive/plans/2026-08-11-project-as-data.md),
which was archived still carrying them, and the test kept asserting against the
archived copy. A live test pinning a frozen document makes the archive a live
specification: the first schema change after the freeze had to amend an archived
file to stay green, which is exactly what "nothing in either archive is current
or edited" forbids. A worked example a test asserts against is an obligation the
plan still owed, so it is lifted here and this document owns it. The archived
copy keeps its 2026-08-11 bytes as history, and nothing asserts against it.

## Tool catalog

`"schema": "umbraflow-tool-catalog/v1"`. One complete `ToolDescriptor` per tool:
every bound a plan is judged against is declared here and nowhere else, so a
widened bound moves `tool_catalog_hash` and therefore
`project_registration_hash`.

`argument_schema` and `result_schema` are both `$defs` names resolved inside the
deployment's tool-precondition schema, and both are required on every row. A
call's arguments and the answer that call is judged by are two halves of one
entry's contract; a tool that could omit either would accept or answer bytes
nothing is entitled to judge. `mutability` is `mutating` or `read_only` and
`surface` is `semantic` or `privileged`, both lowercase and both required —
`ToolDescriptor` defaults them to the restricted value in C++, and "absent means
the safe default" is still "absent means".

The binding that says which code answers a call is deliberately **not** here. It
is a member of the ProjectRegistration, beside `tool_catalog_hash` and outside
it, so that a caller entitled to existence-and-schema can verify the served
catalog against the pinned digest, and so that renaming a handler does not move
a contract digest.

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
      "idempotency": "keyed_external",
      "surface": "semantic",
      "argument_schema": "ChooseEventOptionArguments",
      "result_schema": "ChooseEventOptionResult",
      "required_capabilities": [],
      "effect_bounds": [
        {
          "namespaced_type": "chaos.event_resolved",
          "scope_kind": "run",
          "maximum_risk": "medium",
          "payload_schema_hash": "2eb9225d5bf5b9a694158b0b456e8df6512669ad4b50f90f936a0ab0f1615598"
        }
      ],
      "ui_action_bounds": ["chaos.choose_option"],
      "workflow_limits": {
        "maximum_steps": 8,
        "maximum_dispatches": 8,
        "maximum_observations": 64,
        "maximum_waits": 8,
        "maximum_elapsed_ms": 120000
      },
      "timeout_policy": {"maximum_elapsed_ms": 30000, "on_timeout": "reobserve"}
    },
    {
      "name": "chaos.get_battle_state",
      "version": "1",
      "mutability": "read_only",
      "idempotency": "read_safe",
      "surface": "semantic",
      "argument_schema": "ReadBattleStateArguments",
      "result_schema": "ReadBattleStateResult",
      "required_capabilities": [],
      "effect_bounds": [],
      "ui_action_bounds": [],
      "workflow_limits": {
        "maximum_steps": 1,
        "maximum_dispatches": 1,
        "maximum_observations": 8,
        "maximum_waits": 1,
        "maximum_elapsed_ms": 15000
      },
      "timeout_policy": {"maximum_elapsed_ms": 15000, "on_timeout": "stop"}
    }
  ]
}
```

## Journal event schema manifest

`"schema": "umbraflow-journal-event-schema-manifest/v1"`, named for the
registration member that pins it. Each entry is
`{namespaced_event_type, sha256}` — and no `path`. Which file carries which
payload schema is the deployment block's `journal_payload_schemas`; which schema
answers for which event type is this document's, decided by digest. The two
never appear side by side, which is what makes "what is a manifest `path`
relative to" have no answer rather than a badly chosen one.

The loader compares both directions: a digest no supplied file hashes to is
refused, and a supplied file no entry names is refused too. The second direction
is not symmetry for its own sake — a payload schema no entry names reaches
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

## Reconcile payload schema manifest

`"schema": "umbraflow-reconcile-manifest/v1"`. `verdict_member` is a **named**
member rather than a positional read, deliberately: a positional read means a
later schema edit that adds a member silently changes which value is read.

`dispositions` is a list of `{disposition, value}` rather than a map, so that the
framework's five words are the closed vocabulary on one side and the project's
words are free text on the other. It need not be exhaustive over the five — a
project may never produce `diverged` — and a verdict whose value is in no entry
is refused. A project whose words happen to be the framework's still writes them
out, because "absent means identity" is a default.

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
