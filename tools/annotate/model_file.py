"""Canonical RuntimeModel v2 validation and TOML compilation.

This module compiles runtime-model.toml and never reads one back. The only reader
of that file is the trusted Luau parser in modules/task/runtime/project.luau; a
Python reader here would be a second parser for the same bytes.
"""

from __future__ import annotations

import copy
import hashlib
import json
import math
import re
from pathlib import Path
from typing import Any

from .contracts import validate as validate_contract


_SCHEMA_PATH = Path(__file__).resolve().parents[2] / "schema" / "umbraflow-runtime-v2.schema.json"


class SchemaIssue(ValueError):
    def __init__(self, message: str, path: str = "$") -> None:
        super().__init__(f"{path}: {message}")
        self.path = path
        self.message = message


def _schema_issues(value: Any) -> list[SchemaIssue]:
    return [
        SchemaIssue(error["message"], error["path"])
        for error in validate_contract("umbraflow-runtime-v2.schema.json", value)
    ]


def _index(records: list[dict[str, Any]], collection: str, errors: list[SchemaIssue]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for offset, record in enumerate(records):
        identifier = record.get("id")
        if isinstance(identifier, str) and identifier in result:
            errors.append(SchemaIssue(f"duplicate id {identifier!r}", f"$.{collection}[{offset}].id"))
        elif isinstance(identifier, str):
            result[identifier] = record
    return result


def _validate_detector(
    detector: dict[str, Any],
    path: str,
    locators: dict[str, dict[str, Any]],
    readers: dict[str, dict[str, Any]],
    errors: list[SchemaIssue],
) -> None:
    seen: set[str] = set()
    for branch in ("all", "any", "none"):
        for offset, predicate in enumerate(detector.get(branch, [])):
            predicate_path = f"{path}.{branch}[{offset}]"
            key = json.dumps(predicate, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
            if key in seen:
                errors.append(SchemaIssue("predicate is repeated across detector groups", predicate_path))
            seen.add(key)
            if predicate.get("kind") == "locator_present":
                if locators.get(predicate.get("locator")) is None:
                    errors.append(SchemaIssue("predicate refers to a missing locator", f"{predicate_path}.locator"))
            if predicate.get("kind") == "text_equals":
                if readers.get(predicate.get("reader")) is None:
                    errors.append(SchemaIssue("predicate refers to a missing reader", f"{predicate_path}.reader"))
    if not detector.get("all") and not detector.get("any"):
        errors.append(SchemaIssue("needs at least one positive predicate in all/any", path))


def _validate_rect(rect: list[Any], bounds: list[Any], path: str, errors: list[SchemaIssue]) -> None:
    if not all(isinstance(value, int) and not isinstance(value, bool) for value in rect):
        errors.append(SchemaIssue("must contain integer geometry", path))
        return
    x, y, width, height = rect
    if x + width > bounds[0] or y + height > bounds[1]:
        errors.append(SchemaIssue("must stay inside base_resolution", path))


def _validate_asset_path(value: str, path: str, errors: list[SchemaIssue]) -> None:
    if (
        value.startswith("/")
        or "\0" in value
        or "\\" in value
        or re.match(r"^[A-Za-z]:", value)
        or any(part in {"", ".", ".."} for part in value.split("/"))
    ):
        errors.append(SchemaIssue("must be a confined forward-slash path", path))


def _validate_surface_graph(surfaces: dict[str, dict[str, Any]], errors: list[SchemaIssue]) -> None:
    visiting: set[str] = set()
    visited: set[str] = set()

    def visit(identifier: str) -> None:
        if identifier in visiting:
            errors.append(SchemaIssue(f"cover graph contains a cycle at {identifier!r}", "$.surfaces"))
            return
        if identifier in visited:
            return
        visiting.add(identifier)
        for covered in surfaces[identifier]["covers"]:
            if covered in surfaces:
                visit(covered)
        visiting.remove(identifier)
        visited.add(identifier)

    for identifier in surfaces:
        visit(identifier)


def _surface_covers(surfaces: dict[str, dict[str, Any]], upper: str, lower: str) -> bool:
    pending = list(surfaces[upper]["covers"])
    visited: set[str] = set()
    while pending:
        current = pending.pop()
        if current == lower:
            return True
        if current not in visited and current in surfaces:
            visited.add(current)
            pending.extend(surfaces[current]["covers"])
    return False


def _valid_surface_stack(stack: list[str], surfaces: dict[str, dict[str, Any]]) -> bool:
    if not stack or len(stack) != len(set(stack)):
        return False
    for offset, identifier in enumerate(stack):
        surface = surfaces.get(identifier)
        if surface is None:
            return False
        if offset == 0:
            if surface["kind"] != "scene":
                return False
        elif surface["kind"] == "scene" or not _surface_covers(surfaces, identifier, stack[offset - 1]):
            return False
    return True


def validate_runtime_model(model: dict[str, Any]) -> list[dict[str, str]]:
    errors = _schema_issues(model)
    if errors:
        return [{"path": error.path, "message": error.message} for error in errors]
    for field in ("base_resolution", "base_dpi"):
        if not all(isinstance(value, int) and not isinstance(value, bool) for value in model[field]):
            errors.append(SchemaIssue("must contain positive integers", f"$.{field}"))
    targets = _index(model["ui_targets"], "ui_targets", errors)
    locators = _index(model["locators"], "locators", errors)
    readers = _index(model["readers"], "readers", errors)
    surfaces = _index(model["surfaces"], "surfaces", errors)
    bindings = _index(model["bindings"], "bindings", errors)
    _index(model["transitions"], "transitions", errors)
    for offset, locator in enumerate(model["locators"]):
        _validate_asset_path(locator["asset_path"], f"$.locators[{offset}].asset_path", errors)
    for offset, surface in enumerate(model["surfaces"]):
        if surface["kind"] == "scene" and surface["covers"]:
            errors.append(SchemaIssue("a scene cannot cover another surface", f"$.surfaces[{offset}].covers"))
        if surface["kind"] != "scene" and not surface["covers"]:
            errors.append(SchemaIssue("an overlay or interrupt must cover a surface", f"$.surfaces[{offset}].covers"))
        for covered in surface["covers"]:
            if covered not in surfaces:
                errors.append(SchemaIssue("surface covers a missing surface", f"$.surfaces[{offset}].covers"))
            if covered == surface["id"]:
                errors.append(SchemaIssue("surface cannot cover itself", f"$.surfaces[{offset}].covers"))
        for group in ("all", "any", "none"):
            for identity_offset, binding_id in enumerate(surface["identity"][group]):
                binding = bindings.get(binding_id)
                identity_path = f"$.surfaces[{offset}].identity.{group}[{identity_offset}]"
                if binding is None:
                    errors.append(SchemaIssue("surface identity refers to a missing binding", identity_path))
                elif binding["surface"] != surface["id"]:
                    errors.append(SchemaIssue("surface identity binding belongs to another surface", identity_path))
    _validate_surface_graph(surfaces, errors)
    if not any(surface["kind"] == "scene" for surface in model["surfaces"]):
        errors.append(SchemaIssue("needs at least one scene", "$.surfaces"))
    for offset, binding in enumerate(model["bindings"]):
        if binding["surface"] not in surfaces:
            errors.append(SchemaIssue("binding refers to a missing surface", f"$.bindings[{offset}].surface"))
        if binding["ui_target"] not in targets:
            errors.append(SchemaIssue("binding refers to a missing UI target", f"$.bindings[{offset}].ui_target"))
        _validate_rect(
            binding["placement"]["rect"],
            model["base_resolution"],
            f"$.bindings[{offset}].placement.rect",
            errors,
        )
        action_point = binding["placement"].get("action_point")
        if action_point is not None:
            _validate_rect(
                [action_point[0], action_point[1], 1, 1],
                model["base_resolution"],
                f"$.bindings[{offset}].placement.action_point",
                errors,
            )
        if binding["actions"] and action_point is None:
            errors.append(
                SchemaIssue(
                    "an actionable binding requires an action_point",
                    f"$.bindings[{offset}].placement",
                )
            )
        if not binding["actions"] and action_point is not None:
            errors.append(
                SchemaIssue(
                    "a non-actionable binding cannot declare an action_point",
                    f"$.bindings[{offset}].placement",
                )
            )
        _validate_detector(
            binding["detector"],
            f"$.bindings[{offset}].detector",
            locators,
            readers,
            errors,
        )
        target = targets.get(binding["ui_target"])
        if binding["actions"] and target is not None and target["kind"] != "control":
            errors.append(SchemaIssue("a region UI target cannot grant actions", f"$.bindings[{offset}].actions"))
        positive_locators = {
            predicate["locator"]
            for group in ("all", "any")
            for predicate in binding["detector"][group]
            if predicate["kind"] == "locator_present"
        }
        action_ids: set[str] = set()
        for action_offset, action in enumerate(binding["actions"]):
            if action["id"] in action_ids:
                errors.append(
                    SchemaIssue("binding contains a duplicate action id", f"$.bindings[{offset}].actions[{action_offset}].id")
                )
            action_ids.add(action["id"])
            locator = locators.get(action["proof_locator"])
            if locator is None:
                errors.append(
                    SchemaIssue(
                        "action proof locator is missing",
                        f"$.bindings[{offset}].actions[{action_offset}].proof_locator",
                    )
                )
            elif action["proof_locator"] not in positive_locators:
                errors.append(
                    SchemaIssue(
                        "action proof locator must be positive binding evidence",
                        f"$.bindings[{offset}].actions[{action_offset}].proof_locator",
                    )
                )
    for offset, transition in enumerate(model["transitions"]):
        if not _valid_surface_stack(transition["from_surfaces"], surfaces):
            errors.append(SchemaIssue("from_surfaces is not a valid ordered surface stack", f"$.transitions[{offset}].from_surfaces"))
        if not _valid_surface_stack(transition["to_surfaces"], surfaces):
            errors.append(SchemaIssue("to_surfaces is not a valid ordered surface stack", f"$.transitions[{offset}].to_surfaces"))
        binding = bindings.get(transition["trigger"]["binding"])
        action = transition["trigger"]["action"]
        if binding is None or not any(row["id"] == action for row in binding["actions"]):
            errors.append(SchemaIssue("transition trigger is missing", f"$.transitions[{offset}].trigger"))
    return [{"path": error.path, "message": error.message} for error in errors]


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
        return "{ " + ", ".join(
            f"{_toml_key(key)} = {_toml_value(value[key])}" for key in sorted(value)
        ) + " }"
    raise SchemaIssue(f"unsupported TOML value {value!r}")


def _canonical_model(model: dict[str, Any]) -> dict[str, Any]:
    result = copy.deepcopy(model)
    for collection in ("ui_targets", "locators", "readers", "surfaces", "bindings", "transitions"):
        result[collection].sort(key=lambda row: row["id"])
    for locator in result["locators"]:
        locator["threshold"] = 0.0 if locator["threshold"] == 0 else float(locator["threshold"])
    for reader in result["readers"]:
        reader["confidence_floor"] = 0.0 if reader["confidence_floor"] == 0 else float(reader["confidence_floor"])
    detectors = [binding["detector"] for binding in result["bindings"]]
    for detector in detectors:
        for group in ("all", "any", "none"):
            detector[group].sort(key=lambda row: json.dumps(row, sort_keys=True, separators=(",", ":")))
    for surface in result["surfaces"]:
        surface["covers"].sort()
        for group in ("all", "any", "none"):
            surface["identity"][group].sort()
    for binding in result["bindings"]:
        binding["actions"].sort(key=lambda row: row["id"])
    return result


def runtime_model_to_toml(model: dict[str, Any]) -> str:
    model = _canonical_model(model)
    collections = {
        "ui_targets": "ui_target",
        "locators": "locator",
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
    for collection, singular in collections.items():
        for record in sorted(model[collection], key=lambda item: item["id"]):
            lines.extend(["", f"[[{singular}]]"])
            for key in sorted(record):
                lines.append(f"{_toml_key(key)} = {_toml_value(record[key])}")
    return "\n".join(lines) + "\n"


def compile_runtime_toml(model: dict[str, Any]) -> tuple[bytes, str]:
    errors = validate_runtime_model(model)
    if errors:
        raise SchemaIssue("runtime model is invalid: " + "; ".join(row["message"] for row in errors[:6]))
    content = runtime_model_to_toml(model).encode("utf-8")
    return content, hashlib.sha256(content).hexdigest()
