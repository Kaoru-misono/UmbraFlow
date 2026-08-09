"""Canonical schema validation and runtime model compilation for P5.

This module intentionally has no compatibility reader.  Offline records and
runtime records are validated against the repository schemas, then runtime
compilation projects only the canonical runtime collections into TOML.
"""

from __future__ import annotations

import datetime as _datetime
import hashlib
import json
import math
import os
import re
import tempfile
import tomllib
from pathlib import Path
from typing import Any


_IDENTIFIER = re.compile(r"^[a-z][a-z0-9]*(?:[._-][a-z0-9]+)*$")
_SCHEMA_DIR = Path(__file__).resolve().parents[2] / "schema"
_SCHEMA_FILES = {
    "offline": "umbraflow-offline-v1.schema.json",
    "runtime": "umbraflow-runtime-v1.schema.json",
    "api": "umbraflow-annotator-api-v1.schema.json",
}


class SchemaIssue(ValueError):
    """Raised when a schema or semantic contract is violated."""

    def __init__(self, message: str, path: str = "$") -> None:
        super().__init__(f"{path}: {message}")
        self.path = path
        self.message = message


def issue_dict(error: SchemaIssue) -> dict[str, str]:
    return {"path": error.path, "message": error.message}


class CanonicalSchemas:
    """Small stdlib JSON Schema subset used by the frozen repository schemas."""

    def __init__(self, schema_dir: Path | str = _SCHEMA_DIR) -> None:
        self.schema_dir = Path(schema_dir)
        self.documents: dict[str, dict[str, Any]] = {}
        for name, filename in _SCHEMA_FILES.items():
            with (self.schema_dir / filename).open("r", encoding="utf-8") as stream:
                self.documents[name] = json.load(stream)

    def _resolve_ref(self, ref: str, current: str) -> tuple[dict[str, Any], str]:
        if ref.startswith("#/"):
            document_name = current
            pointer = ref[2:].split("/")
        else:
            filename, _, fragment = ref.partition("#")
            document_name = next(
                (name for name, value in _SCHEMA_FILES.items() if value == filename),
                None,
            )
            if document_name is None:
                raise SchemaIssue(f"unsupported schema reference {ref!r}")
            pointer = fragment[1:].split("/") if fragment.startswith("/") else []
        node: Any = self.documents[document_name]
        for part in pointer:
            node = node[part.replace("~1", "/").replace("~0", "~")]
        return node, document_name

    def validate(self, value: Any, schema_name: str, definition: str | None = None) -> list[SchemaIssue]:
        root = self.documents[schema_name]
        if definition is None and "$ref" in root:
            root, schema_name = self._resolve_ref(root["$ref"], schema_name)
        elif definition is not None:
            root = root["$defs"][definition]
        errors: list[SchemaIssue] = []
        self._validate(value, root, schema_name, "$", errors)
        return errors

    def _validate(
        self,
        value: Any,
        schema: dict[str, Any],
        current: str,
        path: str,
        errors: list[SchemaIssue],
    ) -> None:
        if "$ref" in schema:
            referenced, document_name = self._resolve_ref(schema["$ref"], current)
            self._validate(value, referenced, document_name, path, errors)
            return
        if "allOf" in schema:
            for child in schema["allOf"]:
                self._validate(value, child, current, path, errors)
        if "oneOf" in schema or "anyOf" in schema:
            key = "oneOf" if "oneOf" in schema else "anyOf"
            matches = 0
            branch_errors: list[list[SchemaIssue]] = []
            for child in schema[key]:
                child_errors: list[SchemaIssue] = []
                self._validate(value, child, current, path, child_errors)
                branch_errors.append(child_errors)
                if not child_errors:
                    matches += 1
            if (key == "oneOf" and matches != 1) or (key == "anyOf" and matches == 0):
                errors.append(SchemaIssue(f"does not satisfy {key}", path))
            return
        if "not" in schema:
            child_errors: list[SchemaIssue] = []
            self._validate(value, schema["not"], current, path, child_errors)
            if not child_errors:
                errors.append(SchemaIssue("matches forbidden schema", path))
            return
        if "const" in schema and value != schema["const"]:
            errors.append(SchemaIssue(f"must equal {schema['const']!r}", path))
            return
        if "enum" in schema and value not in schema["enum"]:
            errors.append(SchemaIssue(f"must be one of {schema['enum']!r}", path))
            return
        expected = schema.get("type")
        if expected is not None and not self._type_matches(value, expected):
            errors.append(SchemaIssue(f"must be {expected}", path))
            return
        if isinstance(value, str):
            if len(value) < schema.get("minLength", 0):
                errors.append(SchemaIssue("is shorter than minLength", path))
            if "maxLength" in schema and len(value) > schema["maxLength"]:
                errors.append(SchemaIssue("is longer than maxLength", path))
            if "pattern" in schema and re.search(schema["pattern"], value) is None:
                errors.append(SchemaIssue("does not match the required pattern", path))
            if schema.get("format") == "date-time":
                try:
                    _datetime.datetime.fromisoformat(value.replace("Z", "+00:00"))
                except ValueError:
                    errors.append(SchemaIssue("must be an ISO date-time", path))
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            if "minimum" in schema and value < schema["minimum"]:
                errors.append(SchemaIssue("is below minimum", path))
            if "maximum" in schema and value > schema["maximum"]:
                errors.append(SchemaIssue("is above maximum", path))
            if not math.isfinite(value):
                errors.append(SchemaIssue("must be finite", path))
        if isinstance(value, dict):
            for key in schema.get("required", []):
                if key not in value:
                    errors.append(SchemaIssue(f"missing required field {key!r}", path))
            properties = schema.get("properties", {})
            if schema.get("additionalProperties") is False:
                for key in value:
                    if key not in properties:
                        errors.append(SchemaIssue(f"unknown field {key!r}", f"{path}.{key}"))
            for key, child in properties.items():
                if key in value:
                    self._validate(value[key], child, current, f"{path}.{key}", errors)
        if isinstance(value, list):
            if len(value) < schema.get("minItems", 0):
                errors.append(SchemaIssue("has fewer than minItems", path))
            if "maxItems" in schema and len(value) > schema["maxItems"]:
                errors.append(SchemaIssue("has more than maxItems", path))
            if schema.get("uniqueItems"):
                encoded = [json.dumps(item, sort_keys=True, separators=(",", ":")) for item in value]
                if len(encoded) != len(set(encoded)):
                    errors.append(SchemaIssue("must contain unique items", path))
            prefix_items = schema.get("prefixItems", [])
            for index, child in enumerate(prefix_items):
                if index < len(value):
                    self._validate(value[index], child, current, f"{path}[{index}]", errors)
            items = schema.get("items")
            if items is False and len(value) > len(prefix_items):
                errors.append(SchemaIssue("contains items beyond prefixItems", path))
            elif isinstance(items, dict):
                for index in range(len(prefix_items), len(value)):
                    self._validate(value[index], items, current, f"{path}[{index}]", errors)

    @staticmethod
    def _type_matches(value: Any, expected: str | list[str]) -> bool:
        expected_types = [expected] if isinstance(expected, str) else expected
        return any(
            (kind == "object" and isinstance(value, dict))
            or (kind == "array" and isinstance(value, list))
            or (kind == "string" and isinstance(value, str))
            or (kind == "integer" and isinstance(value, int) and not isinstance(value, bool))
            or (kind == "number" and isinstance(value, (int, float)) and not isinstance(value, bool))
            or (kind == "boolean" and isinstance(value, bool))
            or (kind == "null" and value is None)
            for kind in expected_types
        )


def _ids(records: list[dict[str, Any]], field: str = "id") -> tuple[dict[str, dict[str, Any]], list[SchemaIssue]]:
    result: dict[str, dict[str, Any]] = {}
    errors: list[SchemaIssue] = []
    for index, record in enumerate(records):
        identifier = record.get(field)
        if identifier in result:
            errors.append(SchemaIssue(f"duplicate identifier {identifier!r}", f"$[{index}].{field}"))
        elif isinstance(identifier, str):
            result[identifier] = record
    return result, errors


def _candidate_ids(candidate: dict[str, Any]) -> tuple[dict[str, dict[str, Any]], list[SchemaIssue]]:
    all_records = candidate.get("targets", []) + candidate.get("entities", [])
    result: dict[str, dict[str, Any]] = {}
    errors: list[SchemaIssue] = []
    for index, record in enumerate(all_records):
        identifier = record.get("candidate_id")
        if identifier in result:
            errors.append(SchemaIssue(f"duplicate candidate_id {identifier!r}", f"$.entities[{index}].candidate_id"))
        elif isinstance(identifier, str):
            result[identifier] = record
    return result, errors


def validate_runtime_model(model: dict[str, Any], schemas: CanonicalSchemas | None = None) -> list[dict[str, str]]:
    schemas = schemas or CanonicalSchemas()
    errors = schemas.validate(model, "runtime")
    if errors:
        return [issue_dict(error) for error in errors]
    contexts, context_errors = _ids(model["contexts"])
    targets, target_errors = _ids(model["targets"])
    locators, locator_errors = _ids(model["locators"])
    variants, variant_errors = _ids(model["locator_variants"])
    readers, reader_errors = _ids(model["readers"])
    surfaces, surface_errors = _ids(model["surfaces"])
    bindings, binding_errors = _ids(model["bindings"])
    transitions, transition_errors = _ids(model["transitions"])
    errors = [
        *context_errors,
        *target_errors,
        *locator_errors,
        *variant_errors,
        *reader_errors,
        *surface_errors,
        *binding_errors,
        *transition_errors,
    ]
    for target in targets.values():
        for locator in target["locators"]:
            if locator not in locators:
                errors.append(SchemaIssue(f"target refers to missing locator {locator!r}", f"$.targets[{target['id']}]"))
            elif locators[locator]["target"] != target["id"]:
                errors.append(SchemaIssue("locator belongs to another target", f"$.targets[{target['id']}]"))
        for reader in target["readers"]:
            if reader not in readers:
                errors.append(SchemaIssue(f"target refers to missing reader {reader!r}", f"$.targets[{target['id']}]"))
            elif readers[reader]["target"] != target["id"]:
                errors.append(SchemaIssue("reader belongs to another target", f"$.targets[{target['id']}]"))
    for variant in variants.values():
        if variant["locator"] not in locators:
            errors.append(SchemaIssue("variant refers to missing locator", f"$.locator_variants[{variant['id']}]"))
        elif locators[variant["locator"]]["kind"] != "template":
            errors.append(SchemaIssue("variant parent must be a template locator", f"$.locator_variants[{variant['id']}]"))
        elif variant["id"] not in locators[variant["locator"]].get("variants", []):
            errors.append(SchemaIssue("variant is not declared by its parent locator", f"$.locator_variants[{variant['id']}]"))
    for locator in locators.values():
        if locator["kind"] == "template":
            for variant in locator.get("variants", []):
                if variant not in variants:
                    errors.append(SchemaIssue("template refers to missing variant", f"$.locators[{locator['id']}]"))
        if locator["kind"] in {"relative", "collection"}:
            refs = [locator.get("anchor")] if locator["kind"] == "relative" else [locator.get("item_locator")]
            for ref in refs:
                if ref and ref["locator"] not in locators:
                    errors.append(SchemaIssue("locator refers to missing anchor", f"$.locators[{locator['id']}]"))
    for surface in surfaces.values():
        for context in surface["contexts"]:
            if context not in contexts:
                errors.append(SchemaIssue("surface refers to missing context", f"$.surfaces[{surface['id']}]"))
        if surface["kind"] == "overlay":
            for covered in surface["covers"]:
                if covered not in surfaces:
                    errors.append(SchemaIssue("overlay covers missing surface", f"$.surfaces[{surface['id']}]"))
                elif surfaces[covered]["kind"] == "interrupt":
                    errors.append(SchemaIssue("overlay cannot cover an interrupt", f"$.surfaces[{surface['id']}]"))
    surface_bindings: dict[str, list[dict[str, Any]]] = {key: [] for key in surfaces}
    for binding in bindings.values():
        if binding["surface"] not in surfaces:
            errors.append(SchemaIssue("binding refers to missing surface", f"$.bindings[{binding['id']}]"))
        else:
            surface_bindings[binding["surface"]].append(binding)
        if binding["target"] not in targets:
            errors.append(SchemaIssue("binding refers to missing target", f"$.bindings[{binding['id']}]"))
            continue
        target = targets[binding["target"]]
        for reader in binding["readers"]:
            if reader not in readers or readers.get(reader, {}).get("target") != target["id"]:
                errors.append(SchemaIssue("binding reader is not owned by its target", f"$.bindings[{binding['id']}]"))
        for predicate_group in ("all", "any", "none"):
            for predicate in binding["identity"][predicate_group]:
                _validate_predicate(predicate, target, locators, readers, errors, f"$.bindings[{binding['id']}].identity")
        for action in binding["actions"]:
            for ref in _action_locator_refs(action):
                if ref["locator"] not in target["locators"]:
                    errors.append(SchemaIssue("action locator is outside binding target", f"$.bindings[{binding['id']}]"))
            for predicate in action["preconditions"]:
                _validate_predicate(predicate, target, locators, readers, errors, f"$.bindings[{binding['id']}].actions")
    for surface_id, bound in surface_bindings.items():
        if not any(binding["identity"][group] for binding in bound for group in ("all", "any")):
            errors.append(SchemaIssue("surface has no positive identity evidence", f"$.surfaces[{surface_id}]"))
    for transition in transitions.values():
        for pattern_name in ("from", "to"):
            pattern = transition[pattern_name]
            if pattern["context"] not in contexts:
                errors.append(SchemaIssue("transition refers to missing context", f"$.transitions[{transition['id']}].{pattern_name}"))
            for surface in pattern["surfaces"]:
                if surface not in surfaces:
                    errors.append(SchemaIssue("transition refers to missing surface", f"$.transitions[{transition['id']}].{pattern_name}"))
        trigger = transition["trigger"]
        if trigger["kind"] == "action" and (trigger["binding"] not in bindings or not any(
            action["id"] == trigger["action"] for action in bindings.get(trigger["binding"], {}).get("actions", [])
        )):
            errors.append(SchemaIssue("transition action trigger is not declared", f"$.transitions[{transition['id']}.trigger]"))
    return [issue_dict(error) for error in errors]


def _validate_predicate(
    predicate: dict[str, Any],
    target: dict[str, Any],
    locators: dict[str, dict[str, Any]],
    readers: dict[str, dict[str, Any]],
    errors: list[SchemaIssue],
    path: str,
) -> None:
    if predicate["kind"] == "locator_present":
        locator = predicate["locator"]["locator"]
        if locator not in locators or locator not in target["locators"]:
            errors.append(SchemaIssue("predicate locator is outside binding target", path))
        variant = predicate["locator"].get("variant")
        if variant and (variant not in locators.get(locator, {}).get("variants", [])):
            errors.append(SchemaIssue("predicate variant is not owned by locator", path))
    else:
        if predicate["reader"] not in readers or predicate["reader"] not in target["readers"]:
            errors.append(SchemaIssue("predicate reader is outside binding target", path))


def _action_locator_refs(action: dict[str, Any]) -> list[dict[str, Any]]:
    refs: list[dict[str, Any]] = []
    if "locator" in action:
        refs.append(action["locator"])
    if action["kind"] == "drag" and action["destination"]["kind"] == "locator":
        refs.append(action["destination"]["locator"])
    return refs


def validate_candidate_model(
    candidate: dict[str, Any],
    workspace: dict[str, Any] | None = None,
    schemas: CanonicalSchemas | None = None,
) -> list[dict[str, str]]:
    schemas = schemas or CanonicalSchemas()
    errors = schemas.validate(candidate, "offline", "CandidateModel")
    if errors:
        return [issue_dict(error) for error in errors]
    candidate_ids, candidate_errors = _candidate_ids(candidate)
    errors = list(candidate_errors)
    if workspace is not None:
        frame_ids = {frame["id"] for frame in workspace.get("frames", [])}
        evidence_ids = {row["id"] for row in workspace.get("observations", [])}
        evidence_ids.update(row["id"] for row in workspace.get("assertions", []))
        for frame_id in candidate["source_frame_ids"]:
            if frame_id not in frame_ids:
                errors.append(SchemaIssue("candidate refers to missing source frame", "$.source_frame_ids"))
    else:
        evidence_ids = set()
    if workspace is not None:
        observation_ids = {row["id"] for row in workspace.get("observations", [])}
        assertion_ids = {row["id"] for row in workspace.get("assertions", [])}
        for assertion in workspace.get("assertions", []):
            for observation_id in assertion["supporting_observations"]:
                if observation_id not in observation_ids:
                    errors.append(SchemaIssue("assertion refers to missing observation", f"$.assertions[{assertion['id']}]"))
        _ = assertion_ids
    for record in candidate["targets"] + candidate["entities"]:
        for evidence_id in record["evidence_ids"]:
            if workspace is not None and evidence_id not in evidence_ids:
                errors.append(SchemaIssue("candidate refers to missing evidence", f"$.{record['candidate_id']}.evidence_ids"))
        for conflict_id in record.get("conflict_ids", []):
            if conflict_id not in {item["id"] for item in candidate["conflicts"]}:
                errors.append(SchemaIssue("candidate refers to missing conflict", f"$.{record['candidate_id']}.conflict_ids"))
        entity_kind = "target" if record in candidate["targets"] else record["entity_kind"]
        definition = {
            "context": "Context",
            "target": "Target",
            "locator": "Locator",
            "locator_variant": "LocatorVariant",
            "reader": "Reader",
            "surface": "Surface",
            "binding": "Binding",
            "transition": "Transition",
        }[entity_kind]
        errors.extend(issue_dict(error) for error in schemas.validate(record["value"], "runtime", definition))
    for patch in candidate["patches"]:
        for evidence_id in patch["evidence_ids"]:
            if workspace is not None and evidence_id not in evidence_ids:
                errors.append(SchemaIssue("patch refers to missing evidence", f"$.patches[{patch['id']}]"))
        for conflict_id in patch.get("conflict_ids", []):
            if conflict_id not in {item["id"] for item in candidate["conflicts"]}:
                errors.append(SchemaIssue("patch refers to missing conflict", f"$.patches[{patch['id']}]"))
        errors.extend(validate_patch_change(patch["change"], candidate, schemas))
    for conflict in candidate["conflicts"]:
        for subject_id in conflict["subject_ids"]:
            if subject_id not in candidate_ids and subject_id not in evidence_ids:
                errors.append(SchemaIssue("conflict subject is not a candidate or evidence id", f"$.conflicts[{conflict['id']}]"))
        for patch_id in conflict.get("suggested_patch_ids", []):
            if patch_id not in {item["id"] for item in candidate["patches"]}:
                errors.append(SchemaIssue("conflict refers to missing patch", f"$.conflicts[{conflict['id']}]"))
    return [issue_dict(error) if isinstance(error, SchemaIssue) else error for error in errors]


def validate_patch_change(change: dict[str, Any], candidate: dict[str, Any], schemas: CanonicalSchemas | None = None) -> list[dict[str, str]]:
    schemas = schemas or CanonicalSchemas()
    errors: list[SchemaIssue] = []
    kind = change["kind"]
    if kind == "create_entity":
        definition = {
            "context": "Context",
            "target": "Target",
            "locator": "Locator",
            "locator_variant": "LocatorVariant",
            "reader": "Reader",
            "surface": "Surface",
            "binding": "Binding",
            "transition": "Transition",
        }[change["entity_kind"]]
        errors.extend(schemas.validate(change["value"], "runtime", definition))
    elif kind == "set_field":
        if not _find_entity(candidate, change["entity_kind"], change["entity_id"]):
            errors.append(SchemaIssue("set_field target does not exist", f"$.patches.change.entity_id"))
        if change["field"].startswith("_") or change["field"] in {"classification", "review", "provenance"}:
            errors.append(SchemaIssue("field is not a runtime field", "$.patches.change.field"))
    elif kind in {"grant_action", "revoke_action"}:
        binding = _find_entity(candidate, "binding", change["binding_id"])
        errors.extend(schemas.validate(change["action"], "runtime", "Action"))
        if binding is None:
            errors.append(SchemaIssue("action binding does not exist", "$.patches.change.binding_id"))
    elif kind == "merge_entities":
        if not _find_any_entity(candidate, change["survivor_id"]):
            errors.append(SchemaIssue("merge survivor does not exist", "$.patches.change.survivor_id"))
        for identifier in change["merged_ids"]:
            if not _find_any_entity(candidate, identifier):
                errors.append(SchemaIssue("merge member does not exist", "$.patches.change.merged_ids"))
    elif kind == "split_entity":
        if not _find_any_entity(candidate, change["source_id"]):
            errors.append(SchemaIssue("split source does not exist", "$.patches.change.source_id"))
    return [issue_dict(error) for error in errors]


def _find_entity(candidate: dict[str, Any], entity_kind: str, identifier: str) -> dict[str, Any] | None:
    if entity_kind == "target":
        records = candidate.get("targets", [])
        for record in records:
            if record["value"].get("id") == identifier or record["candidate_id"] == identifier:
                return record
        return None
    for record in candidate.get("entities", []):
        if record["entity_kind"] == entity_kind and (record["value"].get("id") == identifier or record["candidate_id"] == identifier):
            return record
    return None


def _find_any_entity(candidate: dict[str, Any], identifier: str) -> dict[str, Any] | None:
    for entity_kind in ("target", "context", "locator", "locator_variant", "reader", "surface", "binding", "transition"):
        found = _find_entity(candidate, entity_kind, identifier)
        if found:
            return found
    return None


def candidate_summary(candidate: dict[str, Any]) -> dict[str, Any]:
    return {
        "id": candidate["id"],
        "project_id": candidate["project_id"],
        "revision": candidate["revision"],
        "status": candidate["status"],
        "open_conflicts": sum(1 for conflict in candidate["conflicts"] if conflict["status"] == "open"),
        "pending_patches": sum(1 for patch in candidate["patches"] if patch["status"] == "proposed"),
    }


def build_runtime_model(candidate: dict[str, Any]) -> dict[str, Any]:
    collections = {
        "contexts": [],
        "targets": [],
        "locators": [],
        "locator_variants": [],
        "readers": [],
        "surfaces": [],
        "bindings": [],
        "transitions": [],
    }
    records: list[tuple[str, dict[str, Any]]] = [("targets", row) for row in candidate.get("targets", [])]
    entity_collection = {
        "context": "contexts",
        "locator": "locators",
        "locator_variant": "locator_variants",
        "reader": "readers",
        "surface": "surfaces",
        "binding": "bindings",
        "transition": "transitions",
    }
    records.extend((entity_collection[row["entity_kind"]], row) for row in candidate.get("entities", []))
    for collection, record in records:
        if record["state"] in {"rejected", "superseded"}:
            continue
        collections[collection].append(record["value"])
    present = {collection: {item["id"] for item in values} for collection, values in collections.items()}
    for patch in candidate.get("patches", []):
        if patch["status"] not in {"accepted", "applied"} or patch["change"]["kind"] != "create_entity":
            continue
        entity_kind = patch["change"]["entity_kind"]
        collection = "targets" if entity_kind == "target" else entity_collection.get(entity_kind)
        if collection is not None and patch["change"]["value"]["id"] not in present[collection]:
            collections[collection].append(patch["change"]["value"])
            present[collection].add(patch["change"]["value"]["id"])
    return {
        "schema_version": 1,
        "base_resolution": candidate.get("base_resolution", [1, 1]),
        "base_dpi": candidate.get("base_dpi", [1, 1]),
        **collections,
    }


def _toml_key(key: str) -> str:
    return key if re.fullmatch(r"[A-Za-z0-9_-]+", key) else json.dumps(key, ensure_ascii=False)


def _toml_value(value: Any) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, str):
        return json.dumps(value, ensure_ascii=False)
    if isinstance(value, int) and not isinstance(value, bool):
        return str(value)
    if isinstance(value, float):
        if not math.isfinite(value):
            raise SchemaIssue("runtime TOML cannot contain non-finite numbers")
        return repr(value)
    if isinstance(value, list):
        return "[" + ", ".join(_toml_value(item) for item in value) + "]"
    if isinstance(value, dict):
        return "{ " + ", ".join(f"{_toml_key(key)} = {_toml_value(item)}" for key, item in value.items()) + " }"
    raise SchemaIssue(f"unsupported TOML value {value!r}")


def runtime_model_to_toml(model: dict[str, Any]) -> str:
    collection_names = {
        "contexts": "context",
        "targets": "target",
        "locators": "locator",
        "locator_variants": "locator_variant",
        "readers": "reader",
        "surfaces": "surface",
        "bindings": "binding",
        "transitions": "transition",
    }
    lines = [
        f"schema_version = {_toml_value(model['schema_version'])}",
        f"base_resolution = {_toml_value(model['base_resolution'])}",
        f"base_dpi = {_toml_value(model['base_dpi'])}",
    ]
    for collection, singular in collection_names.items():
        for record in model[collection]:
            lines.extend(["", f"[[{singular}]]"])
            for key, value in record.items():
                lines.append(f"{_toml_key(key)} = {_toml_value(value)}")
    return "\n".join(lines) + "\n"


def compile_runtime_toml(
    model: dict[str, Any],
    output_path: Path | str | None = None,
    schemas: CanonicalSchemas | None = None,
) -> tuple[bytes, str]:
    errors = validate_runtime_model(model, schemas)
    if errors:
        raise SchemaIssue("runtime model is invalid: " + "; ".join(item["message"] for item in errors[:6]))
    content = runtime_model_to_toml(model).encode("utf-8")
    digest = hashlib.sha256(content).hexdigest()
    if output_path is not None:
        destination = Path(output_path)
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile("wb", delete=False, dir=destination.parent, prefix=f".{destination.name}.") as stream:
            temporary = Path(stream.name)
            stream.write(content)
        os.replace(temporary, destination)
    return content, digest


def load_runtime_toml(path: Path | str, schemas: CanonicalSchemas | None = None) -> dict[str, Any]:
    raw = tomllib.loads(Path(path).read_text(encoding="utf-8"))
    singular_to_plural = {
        "context": "contexts",
        "target": "targets",
        "locator": "locators",
        "locator_variant": "locator_variants",
        "reader": "readers",
        "surface": "surfaces",
        "binding": "bindings",
        "transition": "transitions",
    }
    allowed = {"schema_version", "base_resolution", "base_dpi", *singular_to_plural}
    unknown = set(raw) - allowed
    if unknown:
        raise SchemaIssue(f"unknown runtime TOML field(s): {sorted(unknown)!r}")
    model = {
        "schema_version": raw.get("schema_version"),
        "base_resolution": raw.get("base_resolution"),
        "base_dpi": raw.get("base_dpi"),
    }
    for singular, plural in singular_to_plural.items():
        model[plural] = raw.get(singular, [])
    errors = validate_runtime_model(model, schemas)
    if errors:
        raise SchemaIssue("runtime TOML is invalid: " + "; ".join(item["message"] for item in errors[:6]))
    return model


def validate_workspace(workspace: dict[str, Any], schemas: CanonicalSchemas | None = None) -> list[dict[str, str]]:
    schemas = schemas or CanonicalSchemas()
    errors = schemas.validate(workspace, "offline")
    if errors:
        return [issue_dict(error) for error in errors]
    frame_ids, frame_errors = _ids(workspace["frames"])
    observation_ids, observation_errors = _ids(workspace["observations"])
    assertion_ids, assertion_errors = _ids(workspace["assertions"])
    errors = [*frame_errors, *observation_errors, *assertion_errors]
    for observation in workspace["observations"]:
        if observation["frame_id"] not in frame_ids:
            errors.append(SchemaIssue("observation refers to missing frame", f"$.observations[{observation['id']}]"))
    for assertion in workspace["assertions"]:
        if assertion["frame_id"] not in frame_ids:
            errors.append(SchemaIssue("assertion refers to missing frame", f"$.assertions[{assertion['id']}]"))
        for observation_id in assertion["supporting_observations"]:
            if observation_id not in observation_ids:
                errors.append(SchemaIssue("assertion refers to missing observation", f"$.assertions[{assertion['id']}]"))
    for candidate in workspace["candidates"]:
        errors.extend(SchemaIssue(item["message"], item["path"]) for item in validate_candidate_model(candidate, workspace, schemas))
    return [issue_dict(error) for error in errors]
