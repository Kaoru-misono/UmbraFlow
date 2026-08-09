"""Directory-backed JSONL evidence store for the offline annotation corpus."""

from __future__ import annotations

import json
import os
import tempfile
from pathlib import Path
from typing import Any, Iterable

from .model_file import CanonicalSchemas, SchemaIssue, validate_workspace


class EvidenceStoreError(ValueError):
    """The offline store is malformed or cannot satisfy an operation."""


class EvidenceStore:
    """Store one canonical collection per JSONL file.

    The directory is intentionally separate from the runtime output.  Frame
    assets and observations are read here, never copied into the runtime model.
    """

    FILES = {
        "frames": "frames.jsonl",
        "observations": "observations.jsonl",
        "assertions": "assertions.jsonl",
        "candidates": "candidates.jsonl",
    }

    def __init__(self, root: Path | str, project_id: str, schemas: CanonicalSchemas | None = None) -> None:
        self.root = Path(root)
        self.project_id = project_id
        self.schemas = schemas or CanonicalSchemas()
        self.root.mkdir(parents=True, exist_ok=True)

    def _read_jsonl(self, name: str) -> list[dict[str, Any]]:
        path = self.root / self.FILES[name]
        if not path.exists():
            return []
        records: list[dict[str, Any]] = []
        try:
            with path.open("r", encoding="utf-8") as stream:
                for line_number, line in enumerate(stream, 1):
                    if not line.strip():
                        continue
                    value = json.loads(line)
                    if not isinstance(value, dict):
                        raise EvidenceStoreError(f"{path}:{line_number}: JSONL record must be an object")
                    records.append(value)
        except (OSError, json.JSONDecodeError) as error:
            raise EvidenceStoreError(f"cannot read {path}: {error}") from error
        return records

    def _write_jsonl(self, name: str, records: Iterable[dict[str, Any]]) -> None:
        destination = self.root / self.FILES[name]
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", newline="\n", delete=False, dir=self.root, prefix=f".{destination.name}.") as stream:
            temporary = Path(stream.name)
            for record in records:
                stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
                stream.write("\n")
        os.replace(temporary, destination)

    def workspace(self) -> dict[str, Any]:
        workspace = {
            "schema_version": 1,
            "project_id": self.project_id,
            "frames": self._read_jsonl("frames"),
            "observations": self._read_jsonl("observations"),
            "assertions": self._read_jsonl("assertions"),
            "candidates": self._read_jsonl("candidates"),
        }
        errors = validate_workspace(workspace, self.schemas)
        if errors:
            first = errors[0]
            raise EvidenceStoreError(f"offline workspace is invalid at {first['path']}: {first['message']}")
        return workspace

    def save_collection(self, name: str, records: list[dict[str, Any]]) -> None:
        if name not in self.FILES:
            raise EvidenceStoreError(f"unknown evidence collection {name!r}")
        candidate = {
            "schema_version": 1,
            "project_id": self.project_id,
            "frames": records if name == "frames" else self._read_jsonl("frames"),
            "observations": records if name == "observations" else self._read_jsonl("observations"),
            "assertions": records if name == "assertions" else self._read_jsonl("assertions"),
            "candidates": records if name == "candidates" else self._read_jsonl("candidates"),
        }
        errors = validate_workspace(candidate, self.schemas)
        if errors:
            first = errors[0]
            raise EvidenceStoreError(f"cannot save invalid workspace at {first['path']}: {first['message']}")
        self._write_jsonl(name, records)

    def candidates(self, status: str | None = None) -> list[dict[str, Any]]:
        rows = self._read_jsonl("candidates")
        if status is not None:
            rows = [row for row in rows if row.get("status") == status]
        return sorted(rows, key=lambda row: row.get("id", ""))

    def get_candidate(self, candidate_id: str) -> dict[str, Any]:
        for candidate in self.candidates():
            if candidate.get("id") == candidate_id:
                return candidate
        raise EvidenceStoreError(f"candidate {candidate_id!r} was not found")

    def save_candidate(self, candidate: dict[str, Any]) -> None:
        rows = self.candidates()
        replaced = False
        for index, row in enumerate(rows):
            if row.get("id") == candidate.get("id"):
                rows[index] = candidate
                replaced = True
                break
        if not replaced:
            rows.append(candidate)
        self.save_collection("candidates", rows)

    def append_pipeline_records(self, records: Iterable[dict[str, Any]]) -> None:
        path = self.root / "pipeline.jsonl"
        with path.open("a", encoding="utf-8", newline="\n") as stream:
            for record in records:
                stream.write(json.dumps(record, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
                stream.write("\n")

    def append_decision(self, candidate_id: str, patch_id: str, actor: str, decision: str, comment: str = "") -> None:
        path = self.root / "decisions.jsonl"
        with path.open("a", encoding="utf-8", newline="\n") as stream:
            stream.write(json.dumps({
                "candidate_id": candidate_id,
                "patch_id": patch_id,
                "actor": actor,
                "decision": decision,
                "comment": comment,
            }, ensure_ascii=False, sort_keys=True, separators=(",", ":")))
            stream.write("\n")

    def decision_records(self, candidate_id: str | None = None) -> list[dict[str, Any]]:
        path = self.root / "decisions.jsonl"
        if not path.exists():
            return []
        records: list[dict[str, Any]] = []
        with path.open("r", encoding="utf-8") as stream:
            for line in stream:
                if line.strip():
                    record = json.loads(line)
                    if candidate_id is None or record.get("candidate_id") == candidate_id:
                        records.append(record)
        return records

    def pipeline_records(self, run_id: str | None = None) -> list[dict[str, Any]]:
        path = self.root / "pipeline.jsonl"
        if not path.exists():
            return []
        records: list[dict[str, Any]] = []
        with path.open("r", encoding="utf-8") as stream:
            for line in stream:
                if line.strip():
                    record = json.loads(line)
                    if run_id is None or record.get("run_id") == run_id:
                        records.append(record)
        return records
