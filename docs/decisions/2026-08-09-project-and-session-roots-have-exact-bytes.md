# 2026-08-09 — Project and session roots have exact bytes

## Decision

`ProjectRegistrationManifest` has exactly these fields:

```text
project_registration_format
plugin_id
plugin_hash
tool_catalog_hash
project_state_schema_hash
project_observation_schema_hash
project_tool_precondition_schema_hash
reconcile_payload_schema_manifest_hash
journal_event_schema_manifest_hash
baseline_event_type
project_artifact_roots[] { name, root_hash }
```

`SessionManifest` has exactly these fields:

```text
host_protocol_schema_hash
runtime_model_schema_hash
runtime_model_artifact_root_hash
operator_protocol_schema_hash
project_registration_hash
policy_artifact_hash
agent_profile_hash
```

Both use one byte rule: RFC 8785 JCS, UTF-8, no BOM, no trailing newline, with
the root being `sha256` of the exact manifest bytes. Every field above
participates in its root. Artifact-root names are non-empty, unique, and sorted
by UTF-8 bytes. Version labels are diagnostic only.

`project_registration_format` is a generation, **not** the digest of the
registration schema file.

## Context

One of the four executable specification resolutions. It implements the consumer
main design §12 and Phase 0 requirement for canonical, language-independent
registration and session roots — canonical because two implementations in two
languages must be able to compute the same root, which requires a byte rule and
not merely a field list.

`project_registration_format` replaced a digest that compared a value with
itself: the loader derived the schema digest once and handed the same local both
to the document and to the schema owner judging the document, so it could not
disagree. In exchange it moved every registration root whenever
`schema/umbraflow-project-registration-v1.schema.json` was cosmetically edited.
A generation number says the same thing honestly.

There is no second `project_artifact_roots_manifest_hash` shape; one spelling of
the roots exists.

## Consequences

- Adding, removing or renaming a field is a root change and therefore a contract
  change.
- `journal_envelope_schema_hash` was later removed from `SessionManifest` — see
  [2026-08-15](2026-08-15-session-manifest-drops-journal-envelope-hash.md).
- Changing any product field, disposition or ownership requires a new consumer
  bundle contract version.
