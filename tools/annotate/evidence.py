"""Content-addressed evidence import and evidence-dependent proposals."""

from __future__ import annotations

import dataclasses
import datetime
import hashlib
import json
import struct
import zlib
from pathlib import Path
from typing import Any, Iterable

from .jcs import jcs_bytes, load_exact_jcs
from .safe_paths import (
    confined_relative,
    make_plain_directories,
    read_plain_file,
    require_plain_directory,
    write_new_file,
)


MAXIMUM_MATERIAL_BYTES = 64 * 1024 * 1024
RECORDING_MAGIC = b"UFREC1\n"
EVIDENCE_SCHEMA = "umbraflow-evidence-set/v1"
PROPOSAL_SCHEMA = "umbraflow-authoring-candidates/v1"

CANDIDATE_KINDS = frozenset({"UiTarget", "Fact", "tool", "identity_recipe"})
CANDIDATE_REASONS = frozenset(
    {
        "explicit_declaration",
        "observed_action",
        "observed_value",
        "visual_recurrence",
    }
)


@dataclasses.dataclass(frozen=True)
class EvidenceSet:
    identity: str
    manifest: dict[str, Any]
    manifest_bytes: bytes


def _digest(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _refuse(path: Path, problem: str) -> ValueError:
    return ValueError(f'evidence input "{path}": {problem}')


def _validate_png(path: Path, content: bytes) -> None:
    signature = b"\x89PNG\r\n\x1a\n"
    if not content.startswith(signature):
        raise _refuse(path, "content is not a PNG")

    cursor = len(signature)
    chunks: list[tuple[bytes, bytes]] = []
    while cursor < len(content):
        if len(content) - cursor < 12:
            raise _refuse(path, "PNG ends inside a chunk")
        length = struct.unpack(">I", content[cursor : cursor + 4])[0]
        end = cursor + 12 + length
        if end > len(content):
            raise _refuse(path, "PNG ends inside a chunk")
        kind = content[cursor + 4 : cursor + 8]
        payload = content[cursor + 8 : cursor + 8 + length]
        stated_crc = struct.unpack(">I", content[cursor + 8 + length : end])[0]
        if zlib.crc32(kind + payload) & 0xFFFFFFFF != stated_crc:
            raise _refuse(path, f"PNG {kind.decode('ascii', 'replace')} checksum differs")
        chunks.append((kind, payload))
        cursor = end
        if kind == b"IEND":
            break

    if cursor != len(content) or not chunks or chunks[-1][0] != b"IEND":
        raise _refuse(path, "PNG has no complete IEND chunk")
    if chunks[0][0] != b"IHDR" or len(chunks[0][1]) != 13:
        raise _refuse(path, "PNG has no valid IHDR chunk")

    width, height, bit_depth, colour, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", chunks[0][1]
    )
    if width == 0 or height == 0 or compression != 0 or filtering != 0 or interlace != 0:
        raise _refuse(path, "PNG header is unsupported or incomplete")
    channels = {0: 1, 2: 3, 4: 2, 6: 4}.get(colour)
    if channels is None or bit_depth != 8:
        raise _refuse(path, "PNG colour encoding is unsupported")
    compressed = b"".join(payload for kind, payload in chunks if kind == b"IDAT")
    try:
        pixels = zlib.decompress(compressed)
    except zlib.error as error:
        raise _refuse(path, "PNG pixel stream is corrupt") from error
    expected = height * (1 + width * channels)
    if len(pixels) != expected:
        raise _refuse(path, "PNG pixel stream ends before the declared image")


def _validate_recording(path: Path, content: bytes) -> None:
    if not content.startswith(RECORDING_MAGIC):
        raise _refuse(path, "content is not a UFREC recording")
    cursor = len(RECORDING_MAGIC)
    frames = 0
    while True:
        if len(content) - cursor < 4:
            raise _refuse(path, "recording ends inside a frame header")
        size = struct.unpack(">I", content[cursor : cursor + 4])[0]
        cursor += 4
        if size == 0:
            if cursor != len(content):
                raise _refuse(path, "recording has bytes after its end marker")
            if frames == 0:
                raise _refuse(path, "recording contains no frames")
            return
        if size > len(content) - cursor:
            raise _refuse(path, f"recording ends inside frame {frames + 1}")
        cursor += size
        frames += 1


def _validate_json(path: Path, content: bytes) -> None:
    try:
        json.loads(content.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise _refuse(path, "content is not UTF-8 JSON") from error


def _kind_and_validate(path: Path, content: bytes) -> str:
    suffix = path.suffix.lower()
    if suffix == ".png":
        _validate_png(path, content)
        return "image"
    if suffix == ".ufrec":
        _validate_recording(path, content)
        return "recording"
    if suffix == ".json":
        _validate_json(path, content)
        return "data"
    if suffix in {".cpp", ".csv", ".hpp", ".lua", ".luau", ".toml", ".txt"}:
        return "source"
    raise _refuse(path, f"unsupported material extension {suffix!r}")


def _utc_now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def import_evidence(
    source_root: Path,
    relative_inputs: Iterable[str],
    evidence_root: Path,
    *,
    imported_at: str | None = None,
) -> EvidenceSet:
    root = source_root.absolute()
    require_plain_directory(root)
    rows: list[dict[str, Any]] = []
    for relative in sorted(relative_inputs):
        confined = confined_relative(relative)
        path = root.joinpath(*confined.parts)
        try:
            content = read_plain_file(path, maximum=MAXIMUM_MATERIAL_BYTES)
        except Exception as error:
            raise _refuse(path, str(error)) from error
        kind = _kind_and_validate(path, content)
        digest = _digest(content)
        rows.append(
            {
                "evidence_ref": f"sha256:{digest}",
                "kind": kind,
                "provenance": {"source": relative},
                "sha256": digest,
                "size": len(content),
            }
        )
    if not rows:
        raise ValueError("evidence import names no inputs")

    identity_rows = [
        {key: row[key] for key in ("kind", "sha256", "size")}
        for row in rows
    ]
    identity_rows.sort(key=lambda row: (row["sha256"], row["kind"], row["size"]))
    identity = _digest(jcs_bytes({"evidence": identity_rows, "schema": EVIDENCE_SCHEMA}))
    provenance_identity = _digest(
        jcs_bytes([row["provenance"] for row in rows])
    )
    destination = evidence_root.absolute()
    make_plain_directories(destination)
    manifest_path = destination / f"{identity}.{provenance_identity}.evidence.json"
    if manifest_path.exists():
        manifest_bytes = read_plain_file(manifest_path, maximum=MAXIMUM_MATERIAL_BYTES)
        manifest = load_exact_jcs(manifest_bytes)
        return EvidenceSet(identity=identity, manifest=manifest, manifest_bytes=manifest_bytes)

    manifest = {
        "evidence": rows,
        "evidence_set_id": f"sha256:{identity}",
        "imported_at": imported_at or _utc_now(),
        "schema": EVIDENCE_SCHEMA,
    }
    manifest_bytes = jcs_bytes(manifest)
    write_new_file(manifest_path, manifest_bytes)
    return EvidenceSet(identity=identity, manifest=manifest, manifest_bytes=manifest_bytes)


def _candidate_hints(path: Path, content: bytes) -> list[dict[str, Any]]:
    if path.suffix.lower() != ".json":
        return []
    document = json.loads(content.decode("utf-8"))
    if not isinstance(document, dict) or "candidate_hints" not in document:
        return []
    hints = document["candidate_hints"]
    if not isinstance(hints, list):
        raise _refuse(path, "candidate_hints is not an array")
    return hints


def propose_candidates(evidence_set: EvidenceSet, source_root: Path) -> dict[str, Any]:
    root = source_root.absolute()
    require_plain_directory(root)
    candidates: list[dict[str, Any]] = []
    for row in evidence_set.manifest["evidence"]:
        relative = row["provenance"]["source"]
        confined = confined_relative(relative)
        path = root.joinpath(*confined.parts)
        content = read_plain_file(path, maximum=MAXIMUM_MATERIAL_BYTES)
        if _digest(content) != row["sha256"]:
            raise _refuse(path, "bytes no longer match the evidence reference")
        for hint in _candidate_hints(path, content):
            if not isinstance(hint, dict):
                raise _refuse(path, "candidate hint is not an object")
            kind = hint.get("kind")
            reason = hint.get("reason")
            confidence = hint.get("confidence")
            if kind not in CANDIDATE_KINDS:
                raise _refuse(path, f"candidate kind {kind!r} is not bounded")
            if reason not in CANDIDATE_REASONS:
                raise _refuse(path, f"candidate reason {reason!r} is not bounded")
            if (
                not isinstance(confidence, (int, float))
                or isinstance(confidence, bool)
                or not 0.0 <= confidence <= 1.0
            ):
                raise _refuse(path, "candidate confidence is outside [0, 1]")
            proposal = {
                "confidence": confidence,
                "evidence_refs": [row["evidence_ref"]],
                "kind": kind,
                "name": hint.get("name"),
                "reason": reason,
            }
            if not isinstance(proposal["name"], str) or not proposal["name"]:
                raise _refuse(path, "candidate name is empty")
            proposal["candidate_id"] = "sha256:" + _digest(jcs_bytes(proposal))
            candidates.append(proposal)
    candidates.sort(key=lambda candidate: candidate["candidate_id"])
    return {"candidates": candidates, "schema": PROPOSAL_SCHEMA}
