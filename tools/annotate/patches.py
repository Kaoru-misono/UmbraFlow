"""Semantic patch decisions for CandidateModel records."""

from __future__ import annotations

from copy import deepcopy
from typing import Any

from .model_file import CanonicalSchemas, SchemaIssue, validate_patch_change


class PatchDecisionError(ValueError):
    """A semantic patch cannot be applied to the current candidate."""


def _record(candidate: dict[str, Any], entity_kind: str, identifier: str) -> dict[str, Any] | None:
    records = candidate.get("targets", []) if entity_kind == "target" else candidate.get("entities", [])
    for record in records:
        if (entity_kind == "target" or record["entity_kind"] == entity_kind) and (
            record["candidate_id"] == identifier or record["value"].get("id") == identifier
        ):
            return record
    return None


def _any_record(candidate: dict[str, Any], identifier: str) -> dict[str, Any] | None:
    for entity_kind in ("target", "context", "locator", "locator_variant", "reader", "surface", "binding", "transition"):
        result = _record(candidate, entity_kind, identifier)
        if result is not None:
            return result
    return None


def _candidate_id(candidate: dict[str, Any], value_id: str) -> str:
    base = f"{value_id}-candidate"
    existing = {row["candidate_id"] for row in candidate.get("targets", []) + candidate.get("entities", [])}
    if base not in existing:
        return base
    index = 2
    while f"{base}-{index}" in existing:
        index += 1
    return f"{base}-{index}"


def _new_record(
    candidate: dict[str, Any],
    entity_kind: str,
    value: dict[str, Any],
    patch: dict[str, Any],
) -> dict[str, Any]:
    existing = _record(candidate, entity_kind, value["id"])
    if existing is not None:
        existing["state"] = "accepted"
        existing["confidence"] = max(existing["confidence"], patch["confidence"])
        existing["evidence_ids"] = sorted(set(existing["evidence_ids"]) | set(patch["evidence_ids"]))
        return existing
    # The offline CandidateModel has no locator collection; locator creation is
    # carried by the semantic patch and materialized only during compilation.
    if entity_kind == "locator":
        return {"value": deepcopy(value), "state": "accepted"}
    common = {
        "candidate_id": _candidate_id(candidate, value["id"]),
        "state": "accepted",
        "value": deepcopy(value),
        "confidence": patch["confidence"],
        "evidence_ids": list(patch["evidence_ids"]),
        "provenance": deepcopy(patch["provenance"]),
    }
    if entity_kind == "target":
        candidate["targets"].append(common)
    else:
        common["entity_kind"] = entity_kind
        candidate["entities"].append(common)
    return common


def apply_semantic_patch(
    candidate: dict[str, Any],
    patch: dict[str, Any],
    schemas: CanonicalSchemas | None = None,
) -> dict[str, Any]:
    """Apply one explicitly accepted patch and return a changed copy.

    This function never promotes an observation or assertion.  It only applies
    the semantic change encoded by the patch after the caller has made the
    human decision.
    """

    schemas = schemas or CanonicalSchemas()
    if patch["status"] != "proposed":
        raise PatchDecisionError(f"patch {patch['id']!r} is not proposed")
    errors = validate_patch_change(patch["change"], candidate, schemas)
    if errors:
        raise PatchDecisionError("invalid patch: " + "; ".join(item["message"] for item in errors))
    changed = deepcopy(candidate)
    change = patch["change"]
    kind = change["kind"]
    if kind == "create_entity":
        _new_record(changed, change["entity_kind"], change["value"], patch)
    elif kind == "set_field":
        record = _record(changed, change["entity_kind"], change["entity_id"])
        if record is None:
            raise PatchDecisionError("set_field target disappeared")
        parts = change["field"].split(".")
        value: Any = record["value"]
        for part in parts[:-1]:
            if not isinstance(value, dict) or part not in value:
                raise PatchDecisionError(f"set_field path {change['field']!r} does not exist")
            value = value[part]
        if not isinstance(value, dict):
            raise PatchDecisionError(f"set_field path {change['field']!r} is not an object")
        value[parts[-1]] = deepcopy(change["value"])
        record["state"] = "accepted"
    elif kind in {"grant_action", "revoke_action"}:
        record = _record(changed, "binding", change["binding_id"])
        if record is None:
            raise PatchDecisionError("action binding disappeared")
        actions = record["value"]["actions"]
        action_id = change["action"]["id"]
        if kind == "grant_action":
            if not any(action["id"] == action_id for action in actions):
                actions.append(deepcopy(change["action"]))
        else:
            record["value"]["actions"] = [action for action in actions if action["id"] != action_id]
        record["state"] = "accepted"
    elif kind == "merge_entities":
        survivor = _any_record(changed, change["survivor_id"])
        if survivor is None:
            raise PatchDecisionError("merge survivor disappeared")
        for identifier in change["merged_ids"]:
            merged = _any_record(changed, identifier)
            if merged is not None and merged["candidate_id"] != survivor["candidate_id"]:
                merged["state"] = "superseded"
        survivor["state"] = "accepted"
    elif kind == "split_entity":
        source = _any_record(changed, change["source_id"])
        if source is None:
            raise PatchDecisionError("split source disappeared")
        source["state"] = "superseded"
        for identifier in change["part_ids"]:
            part = _any_record(changed, identifier)
            if part is not None:
                part["state"] = "accepted"
    else:
        raise PatchDecisionError(f"unsupported patch kind {kind!r}")
    for item in changed["patches"]:
        if item["id"] == patch["id"]:
            item["status"] = "accepted"
            break
    else:
        raise PatchDecisionError(f"patch {patch['id']!r} is not present")
    changed["revision"] += 1
    return changed


def reject_semantic_patch(candidate: dict[str, Any], patch_id: str, actor: str, comment: str | None = None) -> dict[str, Any]:
    changed = deepcopy(candidate)
    for patch in changed["patches"]:
        if patch["id"] == patch_id:
            if patch["status"] != "proposed":
                raise PatchDecisionError(f"patch {patch_id!r} is not proposed")
            patch["status"] = "rejected"
            patch["review"] = {"actor": actor, **({"comment": comment} if comment is not None else {})}
            changed["revision"] += 1
            return changed
    raise PatchDecisionError(f"patch {patch_id!r} does not exist")


def open_conflict_ids(candidate: dict[str, Any]) -> set[str]:
    return {conflict["id"] for conflict in candidate["conflicts"] if conflict["status"] == "open"}


def candidate_is_ready(candidate: dict[str, Any]) -> bool:
    return not open_conflict_ids(candidate) and not any(
        patch["status"] == "proposed" for patch in candidate["patches"]
    ) and all(
        record["state"] in {"accepted", "rejected", "superseded"}
        for record in candidate["targets"] + candidate["entities"]
    )
