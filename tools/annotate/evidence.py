"""Content-addressed evidence import and evidence-dependent proposals."""

from __future__ import annotations

import dataclasses
import datetime
import hashlib
import json
import subprocess
import struct
import zlib
from pathlib import Path
from typing import Any, Callable, Iterable, Mapping, Sequence

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


@dataclasses.dataclass(frozen=True)
class AcceptancePlan:
    project_executable: Path
    source_root: Path
    build_root: Path
    project_inputs: tuple[str, ...]
    replay_command: tuple[str, ...]


@dataclasses.dataclass(frozen=True)
class AcceptanceResult:
    candidate_id: str
    declaration: Path
    build_stdout: str
    replay_stdout: str


class CandidateExecutionError(RuntimeError):
    pass


@dataclasses.dataclass(frozen=True)
class AmbiguityQuestion:
    question_id: str
    alternatives: tuple[dict[str, str], ...]
    evidence_refs: tuple[str, ...]
    kind: str = "ambiguity"


@dataclasses.dataclass(frozen=True)
class CapabilityExpansionQuestion:
    question_id: str
    capability_kind: str
    capability_name: str
    candidate_ids: tuple[str, ...]
    evidence_refs: tuple[str, ...]
    kind: str = "capability_expansion"


AuthoringQuestion = AmbiguityQuestion | CapabilityExpansionQuestion


class QuestionPolicy:
    """Resolve evidence groups, remembering each question and its group-wide answer."""

    def __init__(self, supported_capabilities: Iterable[tuple[str, str]] = ()) -> None:
        self._supported_capabilities = set(supported_capabilities)
        self._raised: dict[str, AuthoringQuestion] = {}
        self._answers: dict[str, str] = {}

    @staticmethod
    def _question_id(material: Mapping[str, Any]) -> str:
        return "sha256:" + _digest(jcs_bytes(material))

    def answer(self, question_id: str, answer: str) -> None:
        question = self._raised[question_id]
        if isinstance(question, CapabilityExpansionQuestion):
            self._supported_capabilities.add(
                (question.capability_kind, question.capability_name)
            )
        self._answers[question_id] = answer

    def resolve(
        self,
        candidates: Sequence[Mapping[str, Any]],
        ask: Callable[[AuthoringQuestion], None],
    ) -> dict[str, bool]:
        decisions: dict[str, bool] = {}
        groups: dict[str, list[Mapping[str, Any]]] = {}
        for candidate in candidates:
            groups.setdefault(candidate["decision_key"], []).append(candidate)

        for decision_key, group in sorted(groups.items()):
            blocked = False
            for capability in sorted(
                {
                    (required["kind"], required["name"])
                    for candidate in group
                    if (required := candidate.get("required_capability")) is not None
                }
            ):
                if capability in self._supported_capabilities:
                    continue
                candidate_ids = tuple(
                    sorted(
                        candidate["candidate_id"]
                        for candidate in group
                        if candidate.get("required_capability")
                        == {"kind": capability[0], "name": capability[1]}
                    )
                )
                evidence_refs = tuple(
                    sorted(
                        {
                            reference
                            for candidate in group
                            for reference in candidate["evidence_refs"]
                        }
                    )
                )
                question_id = self._question_id(
                    {
                        "candidate_ids": list(candidate_ids),
                        "capability": {"kind": capability[0], "name": capability[1]},
                        "evidence_refs": list(evidence_refs),
                        "kind": "capability_expansion",
                    }
                )
                if question_id not in self._answers:
                    blocked = True
                    if question_id not in self._raised:
                        question = CapabilityExpansionQuestion(
                            question_id,
                            capability[0],
                            capability[1],
                            candidate_ids,
                            evidence_refs,
                        )
                        self._raised[question_id] = question
                        ask(question)
            if blocked:
                continue

            ordered = sorted(group, key=lambda candidate: candidate["candidate_id"])
            if len(ordered) == 1:
                decisions[ordered[0]["candidate_id"]] = True
                continue

            alternatives = tuple(
                {
                    "candidate_id": candidate["candidate_id"],
                    "kind": candidate["kind"],
                    "name": candidate["name"],
                }
                for candidate in ordered
            )
            evidence_refs = tuple(
                sorted(
                    {
                        reference
                        for candidate in ordered
                        for reference in candidate["evidence_refs"]
                    }
                )
            )
            question_id = self._question_id(
                {
                    "alternatives": list(alternatives),
                    "decision_key": decision_key,
                    "evidence_refs": list(evidence_refs),
                    "kind": "ambiguity",
                }
            )
            answer = self._answers.get(question_id)
            if answer is not None:
                decisions.update(
                    (candidate["candidate_id"], candidate["candidate_id"] == answer)
                    for candidate in ordered
                )
            elif question_id not in self._raised:
                question = AmbiguityQuestion(
                    question_id, alternatives, evidence_refs
                )
                self._raised[question_id] = question
                ask(question)
        return decisions


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


def _candidate_declaration(path: Path, hint: Mapping[str, Any]) -> dict[str, str]:
    declaration = hint.get("declaration")
    if (
        not isinstance(declaration, dict)
        or set(declaration) != {"content", "path"}
        or not isinstance(declaration["content"], str)
        or not isinstance(declaration["path"], str)
    ):
        raise _refuse(path, "candidate declaration must carry exactly path and content")
    confined_relative(declaration["path"])
    return {"content": declaration["content"], "path": declaration["path"]}


def _required_capability(path: Path, hint: Mapping[str, Any]) -> dict[str, str] | None:
    capability = hint.get("required_capability")
    if capability is None:
        return None
    if (
        not isinstance(capability, dict)
        or set(capability) != {"kind", "name"}
        or capability["kind"] not in {"verb", "shape"}
        or not isinstance(capability["name"], str)
        or not capability["name"]
    ):
        raise _refuse(path, "required capability must name one verb or shape")
    return {"kind": capability["kind"], "name": capability["name"]}


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
                "decision_key": hint.get("decision_key"),
                "declaration": _candidate_declaration(path, hint),
                "evidence_refs": [row["evidence_ref"]],
                "kind": kind,
                "name": hint.get("name"),
                "reason": reason,
            }
            if not isinstance(proposal["name"], str) or not proposal["name"]:
                raise _refuse(path, "candidate name is empty")
            if (
                not isinstance(proposal["decision_key"], str)
                or not proposal["decision_key"]
            ):
                raise _refuse(path, "candidate decision_key is empty")
            required_capability = _required_capability(path, hint)
            if required_capability is not None:
                proposal["required_capability"] = required_capability
            proposal["candidate_id"] = "sha256:" + _digest(jcs_bytes(proposal))
            candidates.append(proposal)
    candidates.sort(key=lambda candidate: candidate["candidate_id"])
    return {"candidates": candidates, "schema": PROPOSAL_SCHEMA}


def _run_candidate_step(
    candidate_id: str,
    step: str,
    command: Sequence[str],
    source_root: Path,
) -> subprocess.CompletedProcess[str]:
    completed = subprocess.run(
        list(command),
        cwd=source_root,
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise CandidateExecutionError(
            f'candidate "{candidate_id}" {step} failed: {detail}'
        )
    return completed


def accept_candidate(
    candidate: Mapping[str, Any],
    plan: AcceptancePlan,
) -> AcceptanceResult:
    """Accept once: write the declaration, then build and replay without prompting."""

    candidate_id = candidate["candidate_id"]
    declaration = candidate["declaration"]
    relative = confined_relative(declaration["path"])
    source_root = plan.source_root.absolute()
    require_plain_directory(source_root)
    destination = source_root.joinpath(*relative.parts)
    make_plain_directories(destination.parent)
    write_new_file(destination, declaration["content"].encode("utf-8"))

    inputs = tuple(sorted({*plan.project_inputs, relative.as_posix()}))
    initialization_command = [
        str(plan.project_executable),
        "init",
        "--source",
        str(source_root),
        "--build",
        str(plan.build_root.absolute()),
    ]
    for project_input in inputs:
        initialization_command.extend(("--input", project_input))
    _run_candidate_step(
        candidate_id,
        "project init",
        initialization_command,
        source_root,
    )
    build = _run_candidate_step(
        candidate_id,
        "project build",
        (
            str(plan.project_executable),
            "build",
            "--source",
            str(source_root),
            "--build",
            str(plan.build_root.absolute()),
        ),
        source_root,
    )
    replay = _run_candidate_step(
        candidate_id,
        "replay",
        plan.replay_command,
        source_root,
    )
    return AcceptanceResult(
        candidate_id,
        destination,
        build.stdout,
        replay.stdout,
    )
