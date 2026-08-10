"""RFC 8785 canonical JSON: the one canonicalization this package hashes."""

from __future__ import annotations

import json
import math
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


def _member_order_key(name: str) -> bytes:
    """Return the UTF-16 code units RFC 8785 orders member names by.

    Neither byte order nor code-point order: a supplementary code point becomes
    a surrogate pair starting in D800..DBFF and therefore sorts before every
    name beginning in E000..FFFF.
    """

    try:
        return name.encode("utf-16be", errors="strict")
    except UnicodeEncodeError as error:
        raise CanonicalJsonError("JCS member names must be Unicode scalar values") from error


def _shortest_digits(magnitude: float) -> tuple[str, int]:
    """Return the shortest digits that read back as exactly this magnitude.

    With them, the decimal exponent ECMAScript calls `n`, so that the value is
    0.<digits> x 10**n. Seventeen significant digits always suffice for a
    double, so the search is bounded.
    """

    for precision in range(17):
        formatted = f"{magnitude:.{precision}e}"
        if float(formatted) == magnitude:
            mantissa, _, exponent = formatted.partition("e")
            return mantissa.replace(".", ""), int(exponent) + 1
    raise CanonicalJsonError("a double has no shortest round-trip decimal form")


def _number(value: float) -> str:
    """ECMAScript Number::toString, which RFC 8785 section 3.2.2.3 adopts verbatim.

    Its layout is not repr's: no exponent from 1e-6 up to just below 1e21, no
    zero padding in the exponent, no trailing `.0`, and -0 prints as 0. Python's
    own repr disagrees on every one of those, which is why this is spelled out
    rather than delegated.
    """

    if math.isnan(value):
        raise CanonicalJsonError("NaN is not a JSON number")
    if math.isinf(value):
        raise CanonicalJsonError("an infinity is not a JSON number")
    if value == 0.0:
        return "0"

    sign = "-" if value < 0 else ""
    digits, n = _shortest_digits(abs(value))
    k = len(digits)
    if k <= n <= 21:
        return sign + digits + "0" * (n - k)
    if 0 < n <= 21:
        return sign + digits[:n] + "." + digits[n:]
    if -6 < n <= 0:
        return sign + "0." + "0" * -n + digits
    power = n - 1
    mantissa = digits if k == 1 else digits[0] + "." + digits[1:]
    return f"{sign}{mantissa}e{'-' if power < 0 else '+'}{abs(power)}"


def _encode(value: Any) -> str:
    if value is None:
        return "null"
    if value is True:
        return "true"
    if value is False:
        return "false"
    if isinstance(value, int) and not isinstance(value, bool):
        # A Python int is unbounded and a JSON number is a double, so an
        # integer past the exact range would be certified by a content hash as
        # a value no other implementation of this scheme can reproduce.
        if abs(value) > MAX_SAFE_INTEGER:
            raise CanonicalJsonError("JCS integer exceeds the exact I-JSON range")
        return str(value)
    if isinstance(value, float):
        return _number(value)
    if isinstance(value, str):
        return _string(value)
    if isinstance(value, list):
        return "[" + ",".join(_encode(item) for item in value) + "]"
    if isinstance(value, dict):
        if not all(isinstance(key, str) for key in value):
            raise CanonicalJsonError("JCS object keys must be strings")
        keys = sorted(value, key=_member_order_key)
        return "{" + ",".join(f"{_string(key)}:{_encode(value[key])}" for key in keys) + "}"
    raise CanonicalJsonError(f"unsupported JCS value: {type(value).__name__}")


def jcs_bytes(value: Any) -> bytes:
    """Return exact UTF-8 JCS bytes with no BOM or trailing newline."""

    return _encode(value).encode("utf-8")


def _reject_constant(name: str) -> Any:
    raise CanonicalJsonError(f"{name} is not a JSON value")


def load_exact_jcs(content: bytes) -> Any:
    """Parse a JCS document and reject non-canonical bytes."""

    try:
        value = json.loads(content.decode("utf-8"), parse_constant=_reject_constant)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CanonicalJsonError("invalid UTF-8 JSON") from error
    if jcs_bytes(value) != content:
        raise CanonicalJsonError("document is not exact RFC 8785 JCS")
    return value
