"""Minimal stdlib HTTP/CLI backend for the offline annotation decision queue."""

from __future__ import annotations

import argparse
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse
from typing import Any

try:
    from .candidate_model import (  # type: ignore[import-not-found]
        build_runtime_model,
        candidate_summary,
        validate_candidate_model,
        validate_runtime_model,
    )
    from .evidence_store import EvidenceStore, EvidenceStoreError
    from .model_file import CanonicalSchemas, SchemaIssue, compile_runtime_toml
    from .patches import PatchDecisionError, apply_semantic_patch, candidate_is_ready, reject_semantic_patch
except ImportError:  # pragma: no cover - supports ``python tools/annotate/serve.py``
    from candidate_model import (  # type: ignore[no-redef]
        build_runtime_model,
        candidate_summary,
        validate_candidate_model,
        validate_runtime_model,
    )
    from evidence_store import EvidenceStore, EvidenceStoreError  # type: ignore[no-redef]
    from model_file import CanonicalSchemas, SchemaIssue, compile_runtime_toml  # type: ignore[no-redef]
    from patches import PatchDecisionError, apply_semantic_patch, candidate_is_ready, reject_semantic_patch  # type: ignore[no-redef]


CAPABILITIES = [
    "list_candidates",
    "get_candidate",
    "accept_patch",
    "reject_patch",
    "compile_candidate",
    "run_validation",
    "get_conflicts",
    "get_provenance",
]


class BackendError(RuntimeError):
    def __init__(self, code: str, message: str, status: int = 400, retryable: bool = False) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.retryable = retryable

    def problem(self) -> dict[str, Any]:
        return {"code": self.code, "message": self.message, "retryable": self.retryable}


class AnnotationBackend:
    def __init__(self, store: EvidenceStore, runtime_path: Path | str | None = None) -> None:
        self.store = store
        self.runtime_path = Path(runtime_path) if runtime_path is not None else self.store.root / "page-model.toml"
        self.schemas = store.schemas

    def schema_manifest(self) -> dict[str, Any]:
        return {
            "api_version": 1,
            "runtime_schema": "umbraflow-runtime-v1.schema.json",
            "offline_schema": "umbraflow-offline-v1.schema.json",
            "capabilities": CAPABILITIES,
        }

    def list_candidates(self, status: str | None = None, cursor: str | None = None) -> dict[str, Any]:
        rows = self.store.candidates(status)
        start = 0
        if cursor:
            try:
                start = int(cursor)
            except ValueError as error:
                raise BackendError("invalid_cursor", "cursor must be an integer", 400) from error
        page = rows[start : start + 100]
        response: dict[str, Any] = {"items": [candidate_summary(row) for row in page]}
        if start + 100 < len(rows):
            response["next_cursor"] = str(start + 100)
        return response

    def get_candidate(self, candidate_id: str) -> dict[str, Any]:
        try:
            return self.store.get_candidate(candidate_id)
        except EvidenceStoreError as error:
            raise BackendError("not_found", str(error), 404) from error

    def _revision(self, candidate_id: str, reviewed: int) -> dict[str, Any]:
        candidate = self.get_candidate(candidate_id)
        if reviewed != candidate["revision"]:
            raise BackendError("revision_conflict", "candidate revision is stale", 409, True)
        return candidate

    def accept_patch(self, candidate_id: str, patch_id: str, body: dict[str, Any]) -> dict[str, Any]:
        candidate = self._revision(candidate_id, body["candidate_revision"])
        patch = next((item for item in candidate["patches"] if item["id"] == patch_id), None)
        if patch is None:
            raise BackendError("not_found", f"patch {patch_id!r} was not found", 404)
        try:
            changed = apply_semantic_patch(candidate, patch, self.schemas)
        except PatchDecisionError as error:
            raise BackendError("invalid_patch", str(error), 422) from error
        self.store.save_candidate(changed)
        self.store.append_decision(changed["id"], patch_id, body["actor"], "accepted", body.get("comment", ""))
        return {"candidate_id": candidate_id, "patch_id": patch_id, "status": "accepted", "revision": changed["revision"]}

    def reject_patch(self, candidate_id: str, patch_id: str, body: dict[str, Any]) -> dict[str, Any]:
        candidate = self._revision(candidate_id, body["candidate_revision"])
        try:
            changed = reject_semantic_patch(candidate, patch_id, body["actor"], body.get("comment"))
        except PatchDecisionError as error:
            raise BackendError("invalid_patch", str(error), 422) from error
        self.store.save_candidate(changed)
        self.store.append_decision(changed["id"], patch_id, body["actor"], "rejected", body.get("comment", ""))
        return {"candidate_id": candidate_id, "patch_id": patch_id, "status": "rejected", "revision": changed["revision"]}

    def run_validation(self, candidate_id: str, revision: int) -> dict[str, Any]:
        candidate = self._revision(candidate_id, revision)
        workspace = self.store.workspace()
        diagnostics = validate_candidate_model(candidate, workspace, self.schemas)
        runtime = build_runtime_model(candidate)
        runtime_diagnostics = validate_runtime_model(runtime, self.schemas)
        all_diagnostics = diagnostics + runtime_diagnostics
        conflict_ids = [conflict["id"] for conflict in candidate["conflicts"] if conflict["status"] == "open"]
        coverage = self._coverage(workspace)
        valid = not all_diagnostics and not conflict_ids and candidate_is_ready(candidate)
        return {
            "candidate_id": candidate_id,
            "revision": candidate["revision"],
            "valid": valid,
            "conflict_ids": conflict_ids,
            "coverage": coverage,
            "diagnostics": all_diagnostics,
        }

    @staticmethod
    def _coverage(workspace: dict[str, Any]) -> dict[str, int]:
        by_frame: dict[str, list[dict[str, Any]]] = {frame["id"]: [] for frame in workspace["frames"]}
        for assertion in workspace["assertions"]:
            if assertion["claim"]["kind"] in {"surface_identity", "surface_stack"}:
                by_frame.setdefault(assertion["frame_id"], []).append(assertion)
        resolved = ambiguous = unknown = 0
        for assertions in by_frame.values():
            accepted_present = [item for item in assertions if item["review"]["status"] == "accepted" and item["outcome"] == "present"]
            if len(accepted_present) == 1:
                resolved += 1
            elif len(accepted_present) > 1:
                ambiguous += 1
            else:
                unknown += 1
        return {"frames": len(by_frame), "resolved": resolved, "ambiguous": ambiguous, "unknown": unknown}

    def compile_candidate(self, candidate_id: str, body: dict[str, Any]) -> dict[str, Any]:
        candidate = self._revision(candidate_id, body["candidate_revision"])
        validation = self.run_validation(candidate_id, candidate["revision"])
        if not validation["valid"]:
            return {
                "candidate_id": candidate_id,
                "revision": candidate["revision"],
                "valid": False,
                "diagnostics": validation["diagnostics"] + [{"severity": "error", "message": "candidate is not ready for compilation"}],
            }
        runtime = build_runtime_model(candidate)
        try:
            content, digest = compile_runtime_toml(runtime, self.runtime_path if body.get("write", False) else None, self.schemas)
        except SchemaIssue as error:
            return {
                "candidate_id": candidate_id,
                "revision": candidate["revision"],
                "valid": False,
                "diagnostics": [{"severity": "error", "path": error.path, "message": error.message}],
            }
        changed = dict(candidate)
        changed["revision"] += 1
        changed["status"] = "compiled" if body.get("write", False) else "accepted"
        if body.get("write", False):
            changed["patches"] = [
                {**patch, "status": "applied" if patch["status"] == "accepted" else patch["status"]}
                for patch in candidate["patches"]
            ]
        self.store.save_candidate(changed)
        response = {"candidate_id": candidate_id, "revision": changed["revision"], "valid": True, "diagnostics": [], "runtime_model_hash": digest}
        _ = content
        return response

    def get_conflicts(self, candidate_id: str) -> list[dict[str, Any]]:
        return self.get_candidate(candidate_id)["conflicts"]

    def get_provenance(self, candidate_id: str) -> list[dict[str, Any]]:
        candidate = self.get_candidate(candidate_id)
        records: list[dict[str, Any]] = []
        for collection in ("targets", "entities", "patches"):
            for item in candidate[collection]:
                records.append({"source_id": item.get("candidate_id", item.get("id")), "kind": collection[:-1], "provenance": item["provenance"]})
        records.extend(self.store.decision_records(candidate_id))
        records.extend(self.store.pipeline_records())
        return records


class _Handler(BaseHTTPRequestHandler):
    backend: AnnotationBackend

    def _send(self, status: int, value: dict[str, Any]) -> None:
        payload = json.dumps(value, ensure_ascii=False, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def _body(self) -> dict[str, Any]:
        length = int(self.headers.get("Content-Length", "0"))
        value = json.loads(self.rfile.read(length) or b"{}")
        if not isinstance(value, dict):
            raise BackendError("invalid_request", "request body must be an object", 400)
        return value

    def _run(self, callback: Any) -> None:
        try:
            self._send(200, callback())
        except BackendError as error:
            self._send(error.status, error.problem())
        except (EvidenceStoreError, ValueError, json.JSONDecodeError) as error:
            self._send(400, {"code": "invalid_request", "message": str(error), "retryable": False})

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        parts = [unquote(part) for part in parsed.path.split("/") if part]
        query = parse_qs(parsed.query)
        if parts == ["api", "schema"]:
            self._run(self.backend.schema_manifest)
        elif parts == ["api", "candidates"]:
            self._run(lambda: self.backend.list_candidates(query.get("status", [None])[0], query.get("cursor", [None])[0]))
        elif len(parts) == 3 and parts[:2] == ["api", "candidates"]:
            self._run(lambda: self.backend.get_candidate(parts[2]))
        elif len(parts) == 4 and parts[:2] == ["api", "candidates"] and parts[3] in {"conflicts", "provenance"}:
            self._run(lambda: self.backend.get_conflicts(parts[2]) if parts[3] == "conflicts" else self.backend.get_provenance(parts[2]))
        else:
            self._send(404, {"code": "not_found", "message": "endpoint not found", "retryable": False})

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        parts = [unquote(part) for part in parsed.path.split("/") if part]
        try:
            body = self._body()
        except (ValueError, json.JSONDecodeError) as error:
            self._send(400, {"code": "invalid_request", "message": str(error), "retryable": False})
            return
        if len(parts) == 5 and parts[:2] == ["api", "candidates"] and parts[3] == "patches" and parts[4] in {"accept", "reject"}:
            callback = lambda: self.backend.accept_patch(parts[2], body["patch_id"], body) if parts[4] == "accept" else self.backend.reject_patch(parts[2], body["patch_id"], body)
        elif len(parts) == 4 and parts[:2] == ["api", "candidates"] and parts[3] == "compile":
            callback = lambda: self.backend.compile_candidate(parts[2], body)
        elif len(parts) == 4 and parts[:2] == ["api", "candidates"] and parts[3] == "validate":
            callback = lambda: self.backend.run_validation(parts[2], body["revision"])
        else:
            self._send(404, {"code": "not_found", "message": "endpoint not found", "retryable": False})
            return
        self._run(callback)

    def log_message(self, format: str, *args: Any) -> None:
        return


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True, type=Path)
    parser.add_argument("--project-id", default=None)
    parser.add_argument("--runtime", type=Path, default=None)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--request", help="execute one API operation as JSON and exit")
    arguments = parser.parse_args(argv)
    project_id = arguments.project_id or arguments.store.name or "offline-project"
    backend = AnnotationBackend(EvidenceStore(arguments.store, project_id), arguments.runtime)
    if arguments.request:
        request = json.loads(arguments.request)
        operation = request["operation"]
        if operation == "list_candidates":
            result = backend.list_candidates(request.get("status"), request.get("cursor"))
        elif operation == "get_candidate":
            result = backend.get_candidate(request["candidate_id"])
        elif operation in {"accept_patch", "reject_patch"}:
            body = request["body"]
            result = getattr(backend, operation)(request["candidate_id"], request["patch_id"], body)
        elif operation == "compile_candidate":
            result = backend.compile_candidate(request["candidate_id"], request["body"])
        elif operation == "run_validation":
            result = backend.run_validation(request["candidate_id"], request["revision"])
        elif operation == "get_conflicts":
            result = backend.get_conflicts(request["candidate_id"])
        elif operation == "get_provenance":
            result = backend.get_provenance(request["candidate_id"])
        else:
            raise SystemExit(f"unsupported operation: {operation}")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    handler = type("AnnotationHandler", (_Handler,), {"backend": backend})
    server = ThreadingHTTPServer((arguments.host, arguments.port), handler)
    print(f"annotation backend listening on http://{arguments.host}:{arguments.port}", file=sys.stderr)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    finally:
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
