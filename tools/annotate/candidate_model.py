"""Candidate revisions proposed by agents and reviewed by humans."""

from __future__ import annotations

import copy
import re
from typing import Any, Mapping

from .model_file import validate_runtime_model


_FIELDS = {
    "evidence_blob_hashes",
    "id",
    "open_issues",
    "proposed_by",
    "revision",
    "runtime_assets",
    "runtime_model",
}
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_ASSET_TYPES = {"template_png", "template_webp"}


def _string_list(
    candidate: Mapping[str, Any],
    field: str,
    errors: list[dict[str, str]],
    *,
    hashes: bool = False,
    minimum: int = 0,
) -> None:
    value = candidate.get(field)
    if not isinstance(value, list) or not all(isinstance(item, str) and item for item in value):
        errors.append({"path": f"$.{field}", "message": "must contain non-empty strings"})
        return
    if len(value) < minimum:
        errors.append({"path": f"$.{field}", "message": f"must contain at least {minimum} item(s)"})
    if len(value) != len(set(value)):
        errors.append({"path": f"$.{field}", "message": "must not contain duplicates"})
    if hashes and any(_SHA256.fullmatch(item) is None for item in value):
        errors.append({"path": f"$.{field}", "message": "must contain lowercase SHA-256 values"})


def validate_candidate_model(candidate: Mapping[str, Any]) -> list[dict[str, str]]:
    """Validate the complete, immutable content of one candidate revision."""

    errors: list[dict[str, str]] = []
    unknown = set(candidate) - _FIELDS
    missing = _FIELDS - set(candidate)
    if unknown:
        errors.append({"path": "$", "message": f"unknown candidate fields: {sorted(unknown)!r}"})
    if missing:
        errors.append({"path": "$", "message": f"missing candidate fields: {sorted(missing)!r}"})
    for field in ("id", "proposed_by"):
        if not isinstance(candidate.get(field), str) or not candidate.get(field):
            errors.append({"path": f"$.{field}", "message": "must be a non-empty string"})
    revision = candidate.get("revision")
    if not isinstance(revision, int) or isinstance(revision, bool) or revision <= 0:
        errors.append({"path": "$.revision", "message": "must be a positive integer"})
    _string_list(candidate, "evidence_blob_hashes", errors, hashes=True, minimum=1)
    _string_list(candidate, "open_issues", errors)
    model = candidate.get("runtime_model")
    if isinstance(model, dict):
        errors.extend(validate_runtime_model(model))
    else:
        errors.append({"path": "$.runtime_model", "message": "must be an object"})
    runtime_assets = candidate.get("runtime_assets")
    asset_paths: set[str] = set()
    if not isinstance(runtime_assets, list):
        errors.append({"path": "$.runtime_assets", "message": "must be an array"})
    else:
        for offset, asset in enumerate(runtime_assets):
            path = f"$.runtime_assets[{offset}]"
            if not isinstance(asset, dict) or set(asset) != {"asset_type", "path", "sha256"}:
                errors.append(
                    {
                        "path": path,
                        "message": "must contain exactly asset_type, path, and sha256",
                    }
                )
                continue
            asset_path = asset["path"]
            if (
                not isinstance(asset_path, str)
                or not asset_path.startswith("assets/")
                or "\\" in asset_path
                or any(part in {"", ".", ".."} for part in asset_path.split("/"))
            ):
                errors.append({"path": f"{path}.path", "message": "must be a confined assets/ path"})
            elif asset_path in asset_paths:
                errors.append({"path": f"{path}.path", "message": "must be unique"})
            else:
                asset_paths.add(asset_path)
            if not isinstance(asset["sha256"], str) or _SHA256.fullmatch(asset["sha256"]) is None:
                errors.append({"path": f"{path}.sha256", "message": "must be a lowercase SHA-256"})
            if asset["asset_type"] not in _ASSET_TYPES:
                errors.append(
                    {
                        "path": f"{path}.asset_type",
                        "message": "must be an explicit deployable template image type",
                    }
                )
    if isinstance(model, dict) and isinstance(runtime_assets, list):
        referenced = {
            locator.get("asset_path")
            for locator in model.get("locators", [])
            if isinstance(locator, dict) and locator.get("kind") == "template"
        }
        if asset_paths != referenced:
            errors.append(
                {
                    "path": "$.runtime_assets",
                    "message": "must exactly close over RuntimeModel template asset paths",
                }
            )
    return errors


def build_runtime_model(candidate: Mapping[str, Any]) -> dict[str, Any]:
    return copy.deepcopy(candidate["runtime_model"])


def candidate_summary(candidate: Mapping[str, Any]) -> dict[str, Any]:
    return {
        "id": candidate["id"],
        "revision": candidate["revision"],
        "proposed_by": candidate["proposed_by"],
        "open_issue_count": len(candidate["open_issues"]),
    }
