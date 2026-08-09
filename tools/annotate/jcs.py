"""RFC 8785 canonical JSON for the integer-only authoring manifests."""

from __future__ import annotations

import json
from typing import Any


MAX_SAFE_INTEGER = (1 << 53) - 1


class CanonicalJsonError(ValueError):
    """A value is outside the exact I-JSON subset used by Umbraflow."""


def _string(value: str) -> str:
    try:
        value.encode("utf-8", errors="strict")
        value.encode("utf-16be", errors="strict")
    except UnicodeEncodeError as error:
        raise CanonicalJsonError("JCS strings must contain Unicode scalar values") from error
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def _encode(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int) and not isinstance(value, bool):
        if abs(value) > MAX_SAFE_INTEGER:
            raise CanonicalJsonError("JCS integer exceeds the exact I-JSON range")
        return str(value)
    if isinstance(value, float):
        raise CanonicalJsonError("Umbraflow manifests do not permit floating-point values")
    if isinstance(value, str):
        return _string(value)
    if isinstance(value, list):
        return "[" + ",".join(_encode(item) for item in value) + "]"
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise CanonicalJsonError("JCS object keys must be strings")
        keys = sorted(value, key=lambda key: key.encode("utf-16be", errors="strict"))
        return "{" + ",".join(f"{_string(key)}:{_encode(value[key])}" for key in keys) + "}"
    raise CanonicalJsonError(f"unsupported JCS value: {type(value).__name__}")


def jcs_bytes(value: Any) -> bytes:
    """Return exact UTF-8 JCS bytes with no BOM or trailing newline."""

    return _encode(value).encode("utf-8")


def load_exact_jcs(content: bytes) -> Any:
    """Parse an integer-only JCS document and reject non-canonical bytes."""

    try:
        value = json.loads(content.decode("utf-8"), parse_float=lambda _: (_ for _ in ()).throw(
            CanonicalJsonError("floating-point values are not permitted")
        ))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CanonicalJsonError("invalid UTF-8 JSON") from error
    if jcs_bytes(value) != content:
        raise CanonicalJsonError("document is not exact RFC 8785 JCS")
    return value
