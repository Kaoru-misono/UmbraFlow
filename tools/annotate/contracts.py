"""Official Draft 2020-12 validation for checked-in Umbraflow contracts."""

from __future__ import annotations

import functools
import json
from pathlib import Path
from typing import Any

from jsonschema import Draft202012Validator


SCHEMA_ROOT = Path(__file__).resolve().parents[2] / "schema"


def _json_path(parts: Any) -> str:
    result = "$"
    for part in parts:
        result += f"[{part}]" if isinstance(part, int) else f".{part}"
    return result


@functools.lru_cache(maxsize=None)
def validator(schema_name: str) -> Draft202012Validator:
    schema = json.loads((SCHEMA_ROOT / schema_name).read_text(encoding="utf-8"))
    Draft202012Validator.check_schema(schema)
    return Draft202012Validator(schema)


def validate(schema_name: str, value: Any) -> list[dict[str, str]]:
    return [
        {"path": _json_path(error.absolute_path), "message": error.message}
        for error in sorted(
            validator(schema_name).iter_errors(value),
            key=lambda item: (list(item.absolute_path), item.message),
        )
    ]


def require_valid(schema_name: str, value: Any, label: str) -> None:
    errors = validate(schema_name, value)
    if errors:
        first = errors[0]
        raise ValueError(f"{label} is invalid at {first['path']}: {first['message']}")
