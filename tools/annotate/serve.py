"""Authenticated loopback Agent API for candidates and checkpoints only."""

from __future__ import annotations

import argparse
import base64
import binascii
import hashlib
import hmac
import json
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Mapping
from urllib.parse import unquote, urlparse

from .candidate_model import build_runtime_model, candidate_summary, validate_candidate_model
from .model_file import SchemaIssue, compile_runtime_toml
from .safe_paths import UnsafePath, paths_overlap, read_plain_file
from .store import (
    APPLICATION_ID,
    DATABASE_NAME,
    SCHEMA_VERSION,
    AnnotationStore,
    BlobUpload,
    Conflict,
    NotFound,
    StoreError,
)


_SCHEMA_ROOT = Path(__file__).resolve().parents[2] / "schema"
_MAX_REQUEST_BYTES = 360 * 1024 * 1024
_TOKEN = re.compile(r"^[A-Za-z0-9_-]{43,128}$")
CAPABILITIES = (
    "get_agent_checkpoint",
    "get_candidate",
    "list_candidates",
    "propose_candidate",
    "save_agent_checkpoint",
    "validate_candidate",
)


class BackendError(RuntimeError):
    def __init__(self, code: str, message: str, status: int = 400, retryable: bool = False) -> None:
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status
        self.retryable = retryable

    def problem(self) -> dict[str, Any]:
        return {"code": self.code, "message": self.message, "retryable": self.retryable}


def _exact_fields(body: Mapping[str, Any], required: set[str]) -> None:
    if set(body) != required:
        raise BackendError("invalid_request", f"request fields must be exactly {sorted(required)!r}")


def load_agent_bearer(path: Path | str, workspace: Path | str) -> str:
    token_path = Path(path).absolute()
    if paths_overlap(token_path, Path(workspace).absolute()):
        raise StoreError("Agent bearer file must be outside the authoring workspace")
    try:
        content = read_plain_file(token_path, maximum=256)
        token = content.decode("ascii")
    except (UnsafePath, UnicodeDecodeError) as error:
        raise StoreError("Agent bearer file must be one small plain ASCII file") from error
    if _TOKEN.fullmatch(token) is None:
        raise StoreError("Agent bearer must be canonical base64url with at least 256 bits")
    try:
        decoded = base64.urlsafe_b64decode(token + "=" * (-len(token) % 4))
    except (ValueError, binascii.Error) as error:
        raise StoreError("Agent bearer is not valid base64url") from error
    if len(decoded) < 32 or base64.urlsafe_b64encode(decoded).decode("ascii").rstrip("=") != token:
        raise StoreError("Agent bearer must be canonical base64url with at least 256 bits")
    return token


class AgentBackend:
    """The complete untrusted mutation surface; no trusted authority methods exist here."""

    def __init__(self, store: AnnotationStore) -> None:
        self.store = store

    def schema_manifest(self) -> dict[str, Any]:
        annotation_schema = _SCHEMA_ROOT / "umbraflow-annotation-workspace-v2.schema.json"
        runtime_schema = _SCHEMA_ROOT / "umbraflow-runtime-v2.schema.json"
        return {
            "annotation_schema_sha256": hashlib.sha256(annotation_schema.read_bytes()).hexdigest(),
            "capabilities": list(CAPABILITIES),
            "database": DATABASE_NAME,
            "sqlite_application_id": APPLICATION_ID,
            "sqlite_user_version": SCHEMA_VERSION,
            "runtime_schema_sha256": hashlib.sha256(runtime_schema.read_bytes()).hexdigest(),
        }

    @staticmethod
    def _decode_blobs(values: Any, allowed_kinds: set[str]) -> list[BlobUpload]:
        if not isinstance(values, list):
            raise BackendError("invalid_request", "blobs must be an array")
        decoded: list[BlobUpload] = []
        for value in values:
            if not isinstance(value, dict) or value.get("kind") not in allowed_kinds:
                raise BackendError("invalid_request", "blob entry has an unsupported kind")
            kind = value["kind"]
            fields = {"content_base64", "kind"} if kind == "evidence" else {
                "asset_type",
                "content_base64",
                "kind",
            }
            if set(value) != fields or not isinstance(value["content_base64"], str):
                raise BackendError("invalid_request", f"{kind} blob entry has the wrong exact shape")
            try:
                content = base64.b64decode(value["content_base64"], validate=True)
            except (ValueError, binascii.Error) as error:
                raise BackendError("invalid_request", "content_base64 is invalid") from error
            if base64.b64encode(content).decode("ascii") != value["content_base64"]:
                raise BackendError("invalid_request", "content_base64 is not canonical")
            decoded.append(
                BlobUpload(
                    kind,
                    content,
                    None if kind == "evidence" else value["asset_type"],
                )
            )
        if len({hashlib.sha256(item.content).digest() for item in decoded}) != len(decoded):
            raise BackendError("invalid_request", "uploaded bytes must have unique identities")
        return decoded

    def propose_candidate(self, body: Mapping[str, Any]) -> dict[str, Any]:
        _exact_fields(body, {"blobs", "candidate", "expected_revision"})
        candidate = body["candidate"]
        expected = body["expected_revision"]
        if not isinstance(candidate, dict):
            raise BackendError("invalid_request", "candidate must be an object")
        if expected is not None and (
            not isinstance(expected, int) or isinstance(expected, bool) or expected <= 0
        ):
            raise BackendError("invalid_request", "expected_revision must be positive or null")
        uploads = self._decode_blobs(body["blobs"], {"evidence", "runtime_asset"})
        try:
            return self.store.commit_candidate_with_uploads(candidate, expected, uploads)
        except Conflict as error:
            raise BackendError("revision_conflict", str(error), 409, True) from error
        except (NotFound, StoreError) as error:
            raise BackendError("invalid_candidate", str(error), 422) from error

    def save_agent_checkpoint(self, body: Mapping[str, Any]) -> dict[str, Any]:
        _exact_fields(body, {"checkpoint", "evidence_blobs", "expected_revision"})
        if not isinstance(body["checkpoint"], dict):
            raise BackendError("invalid_request", "checkpoint must be an object")
        expected = body["expected_revision"]
        if expected is not None and (
            not isinstance(expected, int) or isinstance(expected, bool) or expected <= 0
        ):
            raise BackendError("invalid_request", "expected_revision must be positive or null")
        uploads = self._decode_blobs(body["evidence_blobs"], {"evidence"})
        try:
            return self.store.save_agent_checkpoint_with_uploads(
                body["checkpoint"], expected, uploads
            )
        except Conflict as error:
            raise BackendError("checkpoint_conflict", str(error), 409, True) from error
        except NotFound as error:
            raise BackendError("not_found", str(error), 404) from error
        except StoreError as error:
            raise BackendError("invalid_checkpoint", str(error), 422) from error

    def get_agent_checkpoint(self, job_id: str) -> dict[str, Any]:
        try:
            return {"checkpoint": self.store.get_agent_checkpoint(job_id)}
        except NotFound as error:
            raise BackendError("not_found", str(error), 404) from error

    def list_candidates(self) -> dict[str, Any]:
        return {"items": [candidate_summary(candidate) for candidate in self.store.candidates()]}

    def get_candidate(self, candidate_id: str, revision: int | None = None) -> dict[str, Any]:
        try:
            return {"candidate": self.store.get_candidate(candidate_id, revision)}
        except NotFound as error:
            raise BackendError("not_found", str(error), 404) from error

    def validate_candidate(self, candidate_id: str, revision: int) -> dict[str, Any]:
        candidate = self.get_candidate(candidate_id, revision)["candidate"]
        diagnostics = validate_candidate_model(candidate)
        model_hash = None
        if not diagnostics:
            try:
                _, model_hash = compile_runtime_toml(build_runtime_model(candidate))
            except SchemaIssue as error:
                diagnostics.append({"path": error.path, "message": error.message})
        return {
            "candidate_id": candidate_id,
            "candidate_revision": revision,
            "diagnostics": diagnostics,
            "model_hash": model_hash,
            "open_issues": candidate["open_issues"],
            "valid": not diagnostics,
        }

    def validate_candidate_request(self, candidate_id: str, body: Mapping[str, Any]) -> dict[str, Any]:
        _exact_fields(body, {"candidate_revision"})
        revision = body["candidate_revision"]
        if not isinstance(revision, int) or isinstance(revision, bool) or revision <= 0:
            raise BackendError("invalid_request", "candidate_revision must be positive")
        return self.validate_candidate(candidate_id, revision)


class _Handler(BaseHTTPRequestHandler):
    backend: AgentBackend
    bearer: str
    allowed_hosts: frozenset[str]
    allowed_origins: frozenset[str]

    def _send(self, status: int, value: Any) -> None:
        payload = json.dumps(value, ensure_ascii=False, sort_keys=True).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(payload)

    def _authenticate(self) -> None:
        hosts = self.headers.get_all("Host", failobj=[])
        authorizations = self.headers.get_all("Authorization", failobj=[])
        origins = self.headers.get_all("Origin", failobj=[])
        if len(hosts) != 1 or hosts[0] not in self.allowed_hosts:
            raise BackendError("forbidden", "Host is not the bound loopback endpoint", 403)
        expected = f"Bearer {self.bearer}"
        if len(authorizations) != 1 or not hmac.compare_digest(authorizations[0], expected):
            raise BackendError("unauthorized", "valid Agent bearer required", 401)
        if len(origins) > 1 or (origins and origins[0] not in self.allowed_origins):
            raise BackendError("forbidden", "Origin is not the bound loopback endpoint", 403)
        fetch_site = self.headers.get("Sec-Fetch-Site")
        if fetch_site not in {None, "none", "same-origin"}:
            raise BackendError("forbidden", "cross-site browser requests are forbidden", 403)

    def _body(self) -> dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError as error:
            raise BackendError("invalid_request", "invalid Content-Length") from error
        if length < 0 or length > _MAX_REQUEST_BYTES:
            raise BackendError("invalid_request", "request body is too large", 413)
        value = json.loads(self.rfile.read(length) or b"{}")
        if not isinstance(value, dict):
            raise BackendError("invalid_request", "request body must be an object")
        return value

    def _run(self, callback: Callable[[], Any]) -> None:
        try:
            self._authenticate()
            self._send(200, callback())
        except BackendError as error:
            self._send(error.status, error.problem())
        except (StoreError, UnsafePath, ValueError, json.JSONDecodeError) as error:
            self._send(400, {"code": "invalid_request", "message": str(error), "retryable": False})

    def do_GET(self) -> None:  # noqa: N802
        parts = [unquote(part) for part in urlparse(self.path).path.split("/") if part]
        if parts == ["api", "schema"]:
            callback = self.backend.schema_manifest
        elif parts == ["api", "candidates"]:
            callback = self.backend.list_candidates
        elif len(parts) == 3 and parts[:2] == ["api", "candidates"]:
            callback = lambda: self.backend.get_candidate(parts[2])
        elif len(parts) == 3 and parts[:2] == ["api", "checkpoints"]:
            callback = lambda: self.backend.get_agent_checkpoint(parts[2])
        else:
            callback = lambda: (_ for _ in ()).throw(
                BackendError("not_found", "endpoint not found", 404)
            )
        self._run(callback)

    def do_POST(self) -> None:  # noqa: N802
        parts = [unquote(part) for part in urlparse(self.path).path.split("/") if part]

        def dispatch() -> Any:
            body = self._body()
            if parts == ["api", "candidates"]:
                return self.backend.propose_candidate(body)
            if parts == ["api", "checkpoints"]:
                return self.backend.save_agent_checkpoint(body)
            if len(parts) == 4 and parts[:2] == ["api", "candidates"] and parts[3] == "validate":
                return self.backend.validate_candidate_request(parts[2], body)
            raise BackendError("not_found", "endpoint not found", 404)

        self._run(dispatch)

    def log_message(self, format: str, *args: Any) -> None:
        return


def make_server(backend: AgentBackend, bearer: str, port: int) -> ThreadingHTTPServer:
    handler = type(
        "AnnotationAgentHandler",
        (_Handler,),
        {"backend": backend, "bearer": bearer},
    )
    server = ThreadingHTTPServer(("127.0.0.1", port), handler)
    bound_port = int(server.server_address[1])
    hosts = frozenset({f"127.0.0.1:{bound_port}", f"localhost:{bound_port}"})
    handler.allowed_hosts = hosts
    handler.allowed_origins = frozenset(f"http://{host}" for host in hosts)
    return server


def _one_shot(backend: AgentBackend, request: Mapping[str, Any]) -> Any:
    operation = request.get("operation")
    if operation == "propose_candidate":
        return backend.propose_candidate(request["body"])
    if operation == "save_agent_checkpoint":
        return backend.save_agent_checkpoint(request["body"])
    if operation == "get_agent_checkpoint":
        return backend.get_agent_checkpoint(request["job_id"])
    if operation == "list_candidates":
        return backend.list_candidates()
    if operation == "get_candidate":
        return backend.get_candidate(request["candidate_id"], request.get("candidate_revision"))
    if operation == "validate_candidate":
        return backend.validate_candidate(request["candidate_id"], request["candidate_revision"])
    raise BackendError("invalid_request", f"unsupported Agent operation: {operation!r}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--store", required=True, type=Path)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--bearer-file", type=Path)
    parser.add_argument("--request", help="execute one local Agent operation as JSON and exit")
    arguments = parser.parse_args(argv)
    backend = AgentBackend(AnnotationStore(arguments.store))
    if arguments.request:
        print(json.dumps(_one_shot(backend, json.loads(arguments.request)), ensure_ascii=False, indent=2))
        return 0
    if arguments.bearer_file is None:
        parser.error("--bearer-file is required for the loopback HTTP server")
    bearer = load_agent_bearer(arguments.bearer_file, arguments.store)
    server = make_server(backend, bearer, arguments.port)
    print(
        f"authenticated annotation Agent API listening on http://127.0.0.1:{server.server_address[1]}",
        file=sys.stderr,
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        return 0
    finally:
        server.server_close()


if __name__ == "__main__":
    raise SystemExit(main())
