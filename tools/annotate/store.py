"""Private annotation authority, trusted replay intents, and publication CAS."""

from __future__ import annotations

import contextlib
import datetime as _datetime
import hashlib
import hmac
import json
import os
import re
import sqlite3
import stat
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterator, Mapping, Sequence

from .candidate_model import build_runtime_model, validate_candidate_model
from .contracts import require_valid
from .jcs import CanonicalJsonError, jcs_bytes, load_exact_jcs
from .model_file import SchemaIssue, compile_runtime_toml
from .safe_paths import (
    UnsafePath,
    existing_entries,
    identity,
    is_reparse,
    make_plain_directories,
    open_plain_read,
    read_descriptor,
    read_plain_file,
    remove_plain_tree,
    require_plain_ancestors,
    require_plain_directory,
    require_plain_file,
    write_new_file,
)


DATABASE_NAME = "annotation-workspace.sqlite"
APPLICATION_ID = 0x55464157  # "UFAW"
# The workspace database generation. It is the SQLite user_version this package
# writes and refuses, and the workspace_sqlite_revision every release manifest
# declares, because those are one fact: a deployment principal asking whether it
# understands the database that produced a release is asking for this number.
# Bump it in the same change that alters _SCHEMA_OBJECTS in a way an older
# reader could misread; a comment or a rename inside the DDL is not that.
SCHEMA_VERSION = 2
# The generation of the annotation workspace contract -- the v2 in
# schema/umbraflow-annotation-workspace-v2.schema.json -- that a release
# manifest declares so a Host can say whether it reads that shape. The file's
# bytes are deliberately not the answer: editing prose in the schema would
# otherwise refuse every release already published against it.
ANNOTATION_WORKSPACE_FORMAT = 2
CAPABILITY_VERSION = 1
REPLAY_POLICY_VERSION = 1

_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_CHECKPOINT_STAGES = {"collecting", "proposing", "validating", "complete", "failed"}
_ASSET_TYPES = {"template_png", "template_webp"}
_MAX_RUNTIME_ASSET_BYTES = 268_435_456
_MAX_BLOB_STORE_BYTES = 4 * 1024 * 1024 * 1024
# Candidate revisions are immutable and checkpoints cannot be deleted, so a
# document the untrusted Agent controls needs a ceiling per row and in
# total. Without both, posting large documents under fresh ids grows the
# workspace database irreversibly.
_MAX_DOCUMENT_BYTES = 4 * 1024 * 1024
_MAX_DOCUMENT_STORE_BYTES = 512 * 1024 * 1024
# The schema bounds every Replay Bundle list at 65536 entries; these are the
# same ceilings expressed where the rows are written.
_MAX_BUNDLE_ENTRIES = 65536
_MAX_EVENT_ID_LENGTH = 128
# Frames are the only part of a Replay Bundle the design keeps under a
# retention window, so a bundle may not claim to hold them indefinitely.
_MAX_FRAME_RETENTION_SECONDS = 30 * 24 * 60 * 60
_ANNOTATION_SCHEMA = "umbraflow-annotation-workspace-v2.schema.json"
_SEAL = object()


def _now() -> str:
    return _datetime.datetime.now(_datetime.timezone.utc).isoformat().replace("+00:00", "Z")


def _timestamp(value: Any, name: str) -> _datetime.datetime:
    """One UTC RFC 3339 instant. `format` is annotation-only in Draft 2020-12."""

    if not isinstance(value, str) or not value.endswith("Z"):
        raise StoreError(f"{name} must be one UTC RFC 3339 timestamp ending in Z")
    try:
        parsed = _datetime.datetime.fromisoformat(value[:-1])
    except ValueError as error:
        raise StoreError(f"{name} must be one UTC RFC 3339 timestamp ending in Z") from error
    if parsed.tzinfo is not None:
        raise StoreError(f"{name} must be one UTC RFC 3339 timestamp ending in Z")
    return parsed.replace(tzinfo=_datetime.timezone.utc)


def _now_instant() -> _datetime.datetime:
    """The workspace clock as an instant, so retention reads the same clock the rows are stamped with."""

    return _timestamp(_now(), "workspace clock")


def _sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _hash(value: Any, name: str) -> str:
    if not isinstance(value, str) or _SHA256.fullmatch(value) is None:
        raise StoreError(f"{name} must be a lowercase SHA-256")
    return value


def _positive_integer(value: Any, name: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise StoreError(f"{name} must be a positive integer")
    return value


def _canonical_document(value: Any) -> str:
    """Return the one canonical form: RFC 8785, the same bytes every boundary hashes.

    This was a second canonicalization -- json.dumps(sort_keys=True,
    allow_nan=False) -- that agreed with jcs_bytes only by accident. sort_keys
    orders member names by code point where RFC 8785 orders them by UTF-16 code
    unit, so the two disagree the moment a name outside the Basic Multilingual
    Plane meets one in U+E000..U+FFFF; and json.dumps writes floats and
    integers of any width, which JCS here refuses. Both fed SHA-256 identities,
    and record_project_operation_replay_result computed one hash under each.
    """

    try:
        return jcs_bytes(value).decode("utf-8")
    except CanonicalJsonError as error:
        raise StoreError(f"document must be exact RFC 8785 JSON: {error}") from error


def _object(encoded: str | bytes) -> dict[str, Any]:
    try:
        value = json.loads(encoded)
    except (TypeError, UnicodeDecodeError, json.JSONDecodeError) as error:
        raise StoreError("stored document is not valid JSON") from error
    if not isinstance(value, dict):
        raise StoreError("stored document is not an object")
    return value


def _normalize_sql(value: str) -> str:
    return " ".join(value.split())


def authority_paths_hash(
    human_review: Path | str,
    replay_runner: Path | str,
    publication: Path | str,
    replay_policy: Path | str,
) -> str:
    document = {
        "human_review": os.path.normcase(str(Path(human_review).absolute())),
        "publication": os.path.normcase(str(Path(publication).absolute())),
        "replay_policy": os.path.normcase(str(Path(replay_policy).absolute())),
        "replay_runner": os.path.normcase(str(Path(replay_runner).absolute())),
    }
    return _sha256(jcs_bytes(document))


def _hash_check(name: str) -> str:
    return f"length({name}) = 64 AND {name} NOT GLOB '*[^0-9a-f]*'"


_SCHEMA_OBJECTS: tuple[tuple[str, str], ...] = (
    (
        "authoring_capability_root",
        f"""CREATE TABLE authoring_capability_root(
            singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
            workspace_id TEXT NOT NULL CHECK({_hash_check('workspace_id')}),
            authority_paths_hash TEXT NOT NULL CHECK({_hash_check('authority_paths_hash')}),
            human_review_capability_hash TEXT NOT NULL CHECK({_hash_check('human_review_capability_hash')}),
            replay_runner_capability_hash TEXT NOT NULL CHECK({_hash_check('replay_runner_capability_hash')}),
            publication_capability_hash TEXT NOT NULL CHECK({_hash_check('publication_capability_hash')}),
            replay_policy_hash TEXT NOT NULL CHECK({_hash_check('replay_policy_hash')})
        ) STRICT""",
    ),
    (
        "blobs",
        f"""CREATE TABLE blobs(
            sha256 TEXT PRIMARY KEY CHECK({_hash_check('sha256')}),
            size INTEGER NOT NULL CHECK(size >= 0 AND size <= {_MAX_RUNTIME_ASSET_BYTES}),
            kind TEXT NOT NULL CHECK(kind IN ('evidence','runtime_asset')),
            asset_type TEXT CHECK(asset_type IN ('template_png','template_webp')),
            created_at TEXT NOT NULL,
            CHECK((kind = 'evidence' AND asset_type IS NULL) OR
                  (kind = 'runtime_asset' AND asset_type IS NOT NULL AND size > 0))
        ) STRICT""",
    ),
    (
        "candidate_revisions",
        f"""CREATE TABLE candidate_revisions(
            candidate_id TEXT NOT NULL CHECK(length(candidate_id) > 0),
            revision INTEGER NOT NULL CHECK(revision > 0),
            document TEXT NOT NULL CHECK(length(document) > 1
                AND length(document) <= {_MAX_DOCUMENT_BYTES}),
            document_hash TEXT NOT NULL CHECK({_hash_check('document_hash')}),
            created_at TEXT NOT NULL,
            PRIMARY KEY(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "candidate_heads",
        """CREATE TABLE candidate_heads(
            candidate_id TEXT PRIMARY KEY,
            revision INTEGER NOT NULL CHECK(revision > 0),
            FOREIGN KEY(candidate_id, revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "candidate_blob_refs",
        """CREATE TABLE candidate_blob_refs(
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            sha256 TEXT NOT NULL,
            role TEXT NOT NULL CHECK(role IN ('evidence','runtime_asset')),
            asset_path TEXT,
            PRIMARY KEY(candidate_id, candidate_revision, sha256),
            CHECK((role = 'evidence' AND asset_path IS NULL) OR
                  (role = 'runtime_asset' AND asset_path IS NOT NULL)),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision),
            FOREIGN KEY(sha256) REFERENCES blobs(sha256)
        ) STRICT""",
    ),
    (
        "agent_checkpoints",
        f"""CREATE TABLE agent_checkpoints(
            job_id TEXT PRIMARY KEY CHECK(length(job_id) > 0),
            revision INTEGER NOT NULL CHECK(revision > 0),
            candidate_id TEXT,
            candidate_revision INTEGER,
            document TEXT NOT NULL CHECK(length(document) > 1
                AND length(document) <= {_MAX_DOCUMENT_BYTES}),
            document_hash TEXT NOT NULL CHECK({_hash_check('document_hash')}),
            updated_at TEXT NOT NULL,
            CHECK((candidate_id IS NULL) = (candidate_revision IS NULL)),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "agent_checkpoint_blob_refs",
        """CREATE TABLE agent_checkpoint_blob_refs(
            job_id TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            PRIMARY KEY(job_id, sha256),
            FOREIGN KEY(job_id) REFERENCES agent_checkpoints(job_id),
            FOREIGN KEY(sha256) REFERENCES blobs(sha256)
        ) STRICT""",
    ),
    (
        "review_decisions",
        f"""CREATE TABLE review_decisions(
            decision_id TEXT PRIMARY KEY CHECK({_hash_check('decision_id')}),
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            reviewer_principal TEXT NOT NULL CHECK(length(reviewer_principal) > 0),
            capability_hash TEXT NOT NULL CHECK({_hash_check('capability_hash')}),
            outcome TEXT NOT NULL CHECK(outcome IN ('accepted','rejected')),
            comment TEXT NOT NULL,
            created_at TEXT NOT NULL,
            UNIQUE(candidate_id, candidate_revision),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "replay_result_intents",
        f"""CREATE TABLE replay_result_intents(
            result_id TEXT PRIMARY KEY CHECK({_hash_check('result_id')}),
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            runtime_model_hash TEXT NOT NULL CHECK({_hash_check('runtime_model_hash')}),
            kind TEXT NOT NULL CHECK(kind IN ('frame','transition')),
            corpus_hash TEXT NOT NULL CHECK({_hash_check('corpus_hash')}),
            replay_policy_hash TEXT NOT NULL CHECK({_hash_check('replay_policy_hash')}),
            passed INTEGER NOT NULL CHECK(passed = 1),
            report_hash TEXT NOT NULL CHECK({_hash_check('report_hash')}),
            report TEXT NOT NULL CHECK(length(report) > 1),
            runner_principal TEXT NOT NULL CHECK(length(runner_principal) > 0),
            capability_hash TEXT NOT NULL CHECK({_hash_check('capability_hash')}),
            created_at TEXT NOT NULL,
            UNIQUE(candidate_id, candidate_revision, runtime_model_hash, kind, corpus_hash, report_hash),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "replay_bundles",
        f"""CREATE TABLE replay_bundles(
            bundle_id TEXT PRIMARY KEY CHECK({_hash_check('bundle_id')}),
            baseline_event_id TEXT NOT NULL CHECK(length(baseline_event_id) > 0
                AND length(baseline_event_id) <= {_MAX_EVENT_ID_LENGTH}),
            session_manifest_hash TEXT NOT NULL CHECK({_hash_check('session_manifest_hash')}),
            journal_prefix_length INTEGER NOT NULL CHECK(journal_prefix_length > 0
                AND journal_prefix_length <= {_MAX_BUNDLE_ENTRIES}),
            frame_count INTEGER NOT NULL CHECK(frame_count >= 0
                AND frame_count <= {_MAX_BUNDLE_ENTRIES}),
            frame_retention_expires_at TEXT,
            document TEXT NOT NULL CHECK(length(document) > 1
                AND length(document) <= {_MAX_DOCUMENT_BYTES}),
            runner_principal TEXT NOT NULL CHECK(length(runner_principal) > 0),
            capability_hash TEXT NOT NULL CHECK({_hash_check('capability_hash')}),
            created_at TEXT NOT NULL,
            CHECK((frame_count = 0) = (frame_retention_expires_at IS NULL))
        ) STRICT""",
    ),
    (
        "replay_bundle_blob_refs",
        """CREATE TABLE replay_bundle_blob_refs(
            bundle_id TEXT NOT NULL,
            sha256 TEXT NOT NULL,
            role TEXT NOT NULL CHECK(role IN ('observation','frame')),
            PRIMARY KEY(bundle_id, sha256, role),
            FOREIGN KEY(bundle_id) REFERENCES replay_bundles(bundle_id),
            FOREIGN KEY(sha256) REFERENCES blobs(sha256)
        ) STRICT""",
    ),
    (
        "project_operation_replay_intents",
        f"""CREATE TABLE project_operation_replay_intents(
            result_id TEXT PRIMARY KEY CHECK({_hash_check('result_id')}),
            replay_bundle_id TEXT NOT NULL,
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            replay_policy_hash TEXT NOT NULL CHECK({_hash_check('replay_policy_hash')}),
            passed INTEGER NOT NULL CHECK(passed = 1),
            report_hash TEXT NOT NULL CHECK({_hash_check('report_hash')}),
            report TEXT NOT NULL CHECK(length(report) > 1),
            runner_principal TEXT NOT NULL CHECK(length(runner_principal) > 0),
            capability_hash TEXT NOT NULL CHECK({_hash_check('capability_hash')}),
            created_at TEXT NOT NULL,
            UNIQUE(candidate_id, candidate_revision, replay_bundle_id, report_hash),
            FOREIGN KEY(replay_bundle_id) REFERENCES replay_bundles(bundle_id),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "publications",
        f"""CREATE TABLE publications(
            publication_id TEXT PRIMARY KEY CHECK({_hash_check('publication_id')}),
            generation INTEGER NOT NULL UNIQUE CHECK(generation > 0),
            predecessor_publication_id TEXT,
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            review_decision_id TEXT NOT NULL,
            runtime_model_hash TEXT NOT NULL CHECK({_hash_check('runtime_model_hash')}),
            runtime_artifact_root_hash TEXT NOT NULL CHECK({_hash_check('runtime_artifact_root_hash')}),
            replay_gate_hash TEXT NOT NULL CHECK({_hash_check('replay_gate_hash')}),
            runtime_manifest BLOB NOT NULL CHECK(length(runtime_manifest) > 1),
            release_manifest BLOB NOT NULL CHECK(length(release_manifest) > 1),
            export_name TEXT NOT NULL UNIQUE CHECK({_hash_check('export_name')}),
            publisher_principal TEXT NOT NULL CHECK(length(publisher_principal) > 0),
            publication_capability_hash TEXT NOT NULL CHECK({_hash_check('publication_capability_hash')}),
            created_at TEXT NOT NULL,
            FOREIGN KEY(predecessor_publication_id) REFERENCES publications(publication_id),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision),
            FOREIGN KEY(review_decision_id) REFERENCES review_decisions(decision_id)
        ) STRICT""",
    ),
    (
        "replay_attestations",
        f"""CREATE TABLE replay_attestations(
            attestation_id TEXT PRIMARY KEY CHECK({_hash_check('attestation_id')}),
            replay_result_id TEXT NOT NULL UNIQUE,
            publication_id TEXT NOT NULL,
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            runtime_model_hash TEXT NOT NULL CHECK({_hash_check('runtime_model_hash')}),
            kind TEXT NOT NULL CHECK(kind IN ('frame','transition')),
            corpus_hash TEXT NOT NULL CHECK({_hash_check('corpus_hash')}),
            replay_policy_hash TEXT NOT NULL CHECK({_hash_check('replay_policy_hash')}),
            passed INTEGER NOT NULL CHECK(passed = 1),
            report_hash TEXT NOT NULL CHECK({_hash_check('report_hash')}),
            runner_principal TEXT NOT NULL CHECK(length(runner_principal) > 0),
            capability_hash TEXT NOT NULL CHECK({_hash_check('capability_hash')}),
            created_at TEXT NOT NULL,
            UNIQUE(publication_id, kind),
            FOREIGN KEY(replay_result_id) REFERENCES replay_result_intents(result_id),
            FOREIGN KEY(publication_id) REFERENCES publications(publication_id),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "project_operation_attestations",
        f"""CREATE TABLE project_operation_attestations(
            attestation_id TEXT PRIMARY KEY CHECK({_hash_check('attestation_id')}),
            replay_result_id TEXT NOT NULL UNIQUE,
            publication_id TEXT NOT NULL UNIQUE,
            replay_bundle_id TEXT NOT NULL,
            candidate_id TEXT NOT NULL,
            candidate_revision INTEGER NOT NULL CHECK(candidate_revision > 0),
            replay_policy_hash TEXT NOT NULL CHECK({_hash_check('replay_policy_hash')}),
            passed INTEGER NOT NULL CHECK(passed = 1),
            report_hash TEXT NOT NULL CHECK({_hash_check('report_hash')}),
            runner_principal TEXT NOT NULL CHECK(length(runner_principal) > 0),
            capability_hash TEXT NOT NULL CHECK({_hash_check('capability_hash')}),
            created_at TEXT NOT NULL,
            FOREIGN KEY(replay_result_id)
                REFERENCES project_operation_replay_intents(result_id),
            FOREIGN KEY(publication_id) REFERENCES publications(publication_id),
            FOREIGN KEY(replay_bundle_id) REFERENCES replay_bundles(bundle_id),
            FOREIGN KEY(candidate_id, candidate_revision)
                REFERENCES candidate_revisions(candidate_id, revision)
        ) STRICT""",
    ),
    (
        "published_head",
        """CREATE TABLE published_head(
            singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
            publication_id TEXT,
            generation INTEGER NOT NULL CHECK(generation >= 0),
            CHECK((publication_id IS NULL) = (generation = 0)),
            FOREIGN KEY(publication_id) REFERENCES publications(publication_id)
        ) STRICT""",
    ),
    (
        "candidate_revision_document_hash",
        """CREATE UNIQUE INDEX candidate_revision_document_hash
            ON candidate_revisions(candidate_id, document_hash)""",
    ),
    (
        "publication_candidate_revision",
        """CREATE UNIQUE INDEX publication_candidate_revision
            ON publications(candidate_id, candidate_revision)""",
    ),
)


def _immutable_triggers(table: str) -> tuple[tuple[str, str], tuple[str, str]]:
    return (
        (
            f"{table}_immutable_update",
            f"""CREATE TRIGGER {table}_immutable_update BEFORE UPDATE ON {table}
                BEGIN SELECT RAISE(ABORT, '{table} is immutable'); END""",
        ),
        (
            f"{table}_immutable_delete",
            f"""CREATE TRIGGER {table}_immutable_delete BEFORE DELETE ON {table}
                BEGIN SELECT RAISE(ABORT, '{table} is immutable'); END""",
        ),
    )


_SCHEMA_OBJECTS += tuple(
    item
    for table in (
        "authoring_capability_root",
        "candidate_revisions",
        "candidate_blob_refs",
        "review_decisions",
        "replay_result_intents",
        "replay_bundles",
        "replay_bundle_blob_refs",
        "project_operation_replay_intents",
        "publications",
        "replay_attestations",
        "project_operation_attestations",
    )
    for item in _immutable_triggers(table)
)
_SCHEMA_OBJECTS += (
    (
        "blobs_immutable_update",
        """CREATE TRIGGER blobs_immutable_update BEFORE UPDATE ON blobs
            BEGIN SELECT RAISE(ABORT, 'blobs are immutable'); END""",
    ),
    (
        "candidate_heads_no_delete",
        """CREATE TRIGGER candidate_heads_no_delete BEFORE DELETE ON candidate_heads
            BEGIN SELECT RAISE(ABORT, 'candidate heads cannot be deleted'); END""",
    ),
    (
        "agent_checkpoints_no_delete",
        """CREATE TRIGGER agent_checkpoints_no_delete BEFORE DELETE ON agent_checkpoints
            BEGIN SELECT RAISE(ABORT, 'agent checkpoints cannot be deleted'); END""",
    ),
    (
        "published_head_no_delete",
        """CREATE TRIGGER published_head_no_delete BEFORE DELETE ON published_head
            BEGIN SELECT RAISE(ABORT, 'published head cannot be deleted'); END""",
    ),
)


def _require_document_budget(connection: sqlite3.Connection, encoded: str) -> None:
    """Refuse a document that would push the workspace past its total ceiling.

    The per-row CHECK bounds one document; this bounds how many there can be.
    Both are needed because candidate revisions, agent checkpoints and replay
    bundles are all permanent once written, so nothing here can be reclaimed.
    """
    if len(encoded.encode()) > _MAX_DOCUMENT_BYTES:
        raise StoreError("document exceeds its size ceiling")
    # length() counts characters; the ceiling is in bytes. Casting to blob
    # measures what the ceiling is about, so a document of multi-byte text
    # cannot be four times its accounted size.
    total = sum(
        connection.execute(
            f"SELECT coalesce(sum(length(cast(document as blob))), 0) FROM {table}"
        ).fetchone()[0]
        for table in ("candidate_revisions", "agent_checkpoints", "replay_bundles")
    )
    if total + len(encoded.encode()) > _MAX_DOCUMENT_STORE_BYTES:
        raise StoreError("workspace document quota is exhausted")


class StoreError(RuntimeError):
    """The private authoring authority rejected an operation."""


class NotFound(StoreError):
    pass


class Conflict(StoreError):
    pass


@dataclass(frozen=True)
class BlobRef:
    sha256: str
    size: int
    kind: str
    asset_type: str | None = None


@dataclass(frozen=True)
class BlobUpload:
    kind: str
    content: bytes
    asset_type: str | None = None


# The workspace-relative roots that belong to the authoring capability, named
# so that "production may not read them" is asserted about something. They are
# fixed rather than stored: the layout is created by initialize() below, and the
# schema pins each one as a const, so a row could not disagree with the contract
# without one of the two being wrong.
AUTHORING_ROOTS: dict[str, str] = {
    "workspace_database": DATABASE_NAME,
    "candidate_workspace_root": "objects/runtime-artifacts",
    "evidence_blob_root": "blobs/evidence",
    "replay_bundle_root": "replay-bundles",
}


@dataclass(frozen=True)
class AuthoringCapabilityRoot:
    workspace_id: str
    human_review_capability_hash: str
    replay_runner_capability_hash: str
    publication_capability_hash: str
    replay_policy_hash: str

    def __post_init__(self) -> None:
        for field in self.__dataclass_fields__:
            _hash(getattr(self, field), field)

    def document(self) -> dict[str, Any]:
        value = {
            **{field: getattr(self, field) for field in self.__dataclass_fields__},
            **AUTHORING_ROOTS,
        }
        # Validated, not merely shaped to match. Every release manifest stamps
        # this schema's hash, so a root that no longer satisfies it would be
        # published under a claim nothing checked.
        require_valid(_ANNOTATION_SCHEMA, value, "authoring capability root")
        return value


def _descriptor_document(
    path: Path,
    descriptor: int,
    *,
    maximum: int,
) -> tuple[bytes, dict[str, Any]]:
    try:
        opened_before = os.fstat(descriptor)
        current_before = require_plain_file(path)
        content = read_descriptor(descriptor, maximum=maximum)
        opened_after = os.fstat(descriptor)
        current_after = path.lstat()
    except (OSError, UnsafePath) as error:
        raise StoreError(f"trusted descriptor is no longer valid: {path}") from error
    if (
        identity(opened_before) != identity(opened_after)
        or identity(opened_after) != identity(current_before)
        or identity(opened_after) != identity(current_after)
        or opened_after.st_size != len(content)
        or is_reparse(current_after)
    ):
        raise StoreError("trusted descriptor identity changed")
    try:
        document = load_exact_jcs(content)
    except CanonicalJsonError as error:
        raise StoreError(str(error)) from error
    if not isinstance(document, dict):
        raise StoreError("trusted descriptor must contain one exact JCS object")
    return content, document


class _DescriptorAuthority:
    PURPOSE = ""

    # __slots__ so that in-process code cannot bolt a replacement verify_open or
    # sha256 onto an instance. The real gate is still the live descriptor and
    # the DB-pinned hash; this only removes the cheapest way to sidestep them.
    __slots__ = ("path", "_descriptor", "_content", "_document", "sha256", "_guard")

    def __init__(
        self,
        path: Path,
        descriptor: int,
        content: bytes,
        document: Mapping[str, Any],
        *,
        _token: object,
    ) -> None:
        if _token is not _SEAL:
            raise StoreError("trusted authority objects can only come from an open descriptor")
        self.path = path
        self._descriptor = descriptor
        self._content = bytes(content)
        self._document = dict(document)
        self.sha256 = _sha256(content)
        self._guard = threading.Lock()

    @property
    def principal(self) -> str:
        return str(self._document["principal"])

    @property
    def purpose(self) -> str:
        return self.PURPOSE

    def verify_open(self) -> None:
        with self._guard:
            if self._descriptor < 0:
                raise StoreError("trusted authority descriptor is closed")
            content, document = _descriptor_document(self.path, self._descriptor, maximum=16 * 1024)
            expected_fields = {"capability_version", "nonce", "principal", "purpose"}
            if set(document) != expected_fields:
                raise StoreError("trusted capability has the wrong exact shape")
            if document["capability_version"] != CAPABILITY_VERSION:
                raise StoreError("trusted capability version is unsupported")
            _hash(document["nonce"], "capability nonce")
            if not isinstance(document["principal"], str) or not document["principal"]:
                raise StoreError("trusted capability principal is required")
            if document["purpose"] != self.PURPOSE:
                raise StoreError("trusted capability purpose changed")
            if not hmac.compare_digest(content, self._content):
                raise StoreError("trusted capability exact bytes changed")
            if _sha256(content) != self.sha256 or document != self._document:
                raise StoreError("trusted capability identity changed")

    def close(self) -> None:
        with self._guard:
            if self._descriptor >= 0:
                os.close(self._descriptor)
                self._descriptor = -1

    def __enter__(self) -> _DescriptorAuthority:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except BaseException:
            pass


class HumanReviewCapability(_DescriptorAuthority):
    PURPOSE = "human-review"


class ReplayRunnerCapability(_DescriptorAuthority):
    PURPOSE = "replay-runner"


class PublicationCapability(_DescriptorAuthority):
    PURPOSE = "publication"


def _open_capability(path: Path | str, capability_type: type[_DescriptorAuthority]) -> Any:
    requested = Path(path).absolute()
    try:
        descriptor = open_plain_read(requested)
    except UnsafePath as error:
        raise StoreError("trusted capability file cannot be opened") from error
    instance = None
    try:
        metadata = os.fstat(descriptor)
        if os.name != "nt" and metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise StoreError("trusted capability file permissions are too broad")
        content, document = _descriptor_document(requested, descriptor, maximum=16 * 1024)
        instance = capability_type(
            requested,
            descriptor,
            content,
            document,
            _token=_SEAL,
        )
        instance.verify_open()
        return instance
    except BaseException:
        # Close through the instance once it exists: it owns the descriptor
        # from construction, and closing the bare number here as well would
        # close it twice -- in a process opening files concurrently, the second
        # close lands on whatever reused it.
        if instance is None:
            os.close(descriptor)
        else:
            instance.close()
        raise


def open_human_review_capability(path: Path | str) -> HumanReviewCapability:
    return _open_capability(path, HumanReviewCapability)


def open_replay_runner_capability(path: Path | str) -> ReplayRunnerCapability:
    return _open_capability(path, ReplayRunnerCapability)


def open_publication_capability(path: Path | str) -> PublicationCapability:
    return _open_capability(path, PublicationCapability)


class ReplayPolicy:
    # Same reasoning as _DescriptorAuthority: verify_open is the on-disk
    # tamper re-check, so it must not be replaceable on an instance.
    __slots__ = ("path", "_descriptor", "_content", "_document", "exact_hash", "_guard")

    def __init__(
        self,
        path: Path,
        descriptor: int,
        content: bytes,
        document: Mapping[str, Any],
        *,
        _token: object,
    ) -> None:
        if _token is not _SEAL:
            raise StoreError("replay policy can only come from an open descriptor")
        self.path = path
        self._descriptor = descriptor
        self._content = bytes(content)
        self._document = dict(document)
        self.exact_hash = _sha256(content)
        self._guard = threading.Lock()

    @property
    def frame_corpus_hash(self) -> str:
        return str(self._document["frame_corpus_hash"])

    @property
    def transition_corpus_hash(self) -> str:
        return str(self._document["transition_corpus_hash"])

    def corpus_for(self, kind: str) -> str:
        if kind == "frame":
            return self.frame_corpus_hash
        if kind == "transition":
            return self.transition_corpus_hash
        raise StoreError(f"unknown replay kind {kind!r}")

    def verify_open(self) -> None:
        with self._guard:
            if self._descriptor < 0:
                raise StoreError("replay policy descriptor is closed")
            content, document = _descriptor_document(self.path, self._descriptor, maximum=16 * 1024)
            if set(document) != {
                "frame_corpus_hash",
                "policy_version",
                "transition_corpus_hash",
            }:
                raise StoreError("replay policy has the wrong exact shape")
            if document["policy_version"] != REPLAY_POLICY_VERSION:
                raise StoreError("replay policy version is unsupported")
            _hash(document["frame_corpus_hash"], "frame corpus hash")
            _hash(document["transition_corpus_hash"], "transition corpus hash")
            if not hmac.compare_digest(content, self._content):
                raise StoreError("replay policy exact bytes changed")
            if _sha256(content) != self.exact_hash or document != self._document:
                raise StoreError("replay policy identity changed")

    def close(self) -> None:
        with self._guard:
            if self._descriptor >= 0:
                os.close(self._descriptor)
                self._descriptor = -1

    def __enter__(self) -> ReplayPolicy:
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def __del__(self) -> None:
        try:
            self.close()
        except BaseException:
            pass


def open_replay_policy(path: Path | str) -> ReplayPolicy:
    requested = Path(path).absolute()
    try:
        descriptor = open_plain_read(requested)
    except UnsafePath as error:
        raise StoreError("replay policy cannot be opened") from error
    policy = None
    try:
        metadata = os.fstat(descriptor)
        if os.name != "nt" and metadata.st_mode & (stat.S_IWGRP | stat.S_IWOTH):
            raise StoreError("replay policy file permissions are too broad")
        content, document = _descriptor_document(requested, descriptor, maximum=16 * 1024)
        policy = ReplayPolicy(requested, descriptor, content, document, _token=_SEAL)
        policy.verify_open()
        return policy
    except BaseException:
        if policy is None:
            os.close(descriptor)
        else:
            policy.close()
        raise


_LOCAL_LOCKS: dict[str, threading.RLock] = {}
_LOCAL_LOCKS_GUARD = threading.Lock()


def _local_lock(path: Path) -> threading.RLock:
    key = os.path.normcase(str(path.absolute()))
    with _LOCAL_LOCKS_GUARD:
        return _LOCAL_LOCKS.setdefault(key, threading.RLock())


class AnnotationStore:
    """One exact workspace database plus private, kind-separated objects."""

    def __init__(self, root: Path | str) -> None:
        self.root = Path(root).absolute()
        self.database = self.root / DATABASE_NAME
        self.lock_file = self.root / ".workspace.lock"
        self.evidence_blobs = self.root / "blobs" / "evidence"
        self.runtime_blobs = self.root / "blobs" / "runtime-assets"
        self.objects = self.root / "objects" / "runtime-artifacts"
        self.replay_bundles = self.root / AUTHORING_ROOTS["replay_bundle_root"]
        self.staging = self.root / ".staging"
        self._root_identity = identity(require_plain_directory(self.root))
        self._lock_state = threading.local()
        for directory in (
            self.evidence_blobs,
            self.runtime_blobs,
            *(self.runtime_blobs / value for value in sorted(_ASSET_TYPES)),
            self.objects,
            self.replay_bundles,
            self.staging,
        ):
            require_plain_directory(directory)
        require_plain_file(self.database)
        require_plain_file(self.lock_file)
        self._validate_database()
        self.garbage_collect_unreferenced_blobs()

    @classmethod
    def initialize(
        cls,
        root: Path | str,
        capability_root: AuthoringCapabilityRoot,
        *,
        authority_paths_digest: str,
    ) -> AnnotationStore:
        _hash(authority_paths_digest, "authority paths hash")
        workspace = Path(root).absolute()
        make_plain_directories(workspace)
        database = workspace / DATABASE_NAME
        if os.path.lexists(database):
            raise Conflict(f"authoring database already exists: {database}")
        for directory in (
            workspace / AUTHORING_ROOTS["evidence_blob_root"],
            workspace / "blobs" / "runtime-assets",
            *(workspace / "blobs" / "runtime-assets" / value for value in sorted(_ASSET_TYPES)),
            workspace / AUTHORING_ROOTS["candidate_workspace_root"],
            workspace / AUTHORING_ROOTS["replay_bundle_root"],
            workspace / ".staging",
        ):
            make_plain_directories(directory)
        write_new_file(workspace / ".workspace.lock", b"\0")
        connection = sqlite3.connect(database, timeout=5.0)
        try:
            connection.execute("PRAGMA foreign_keys = ON")
            connection.execute("PRAGMA synchronous = FULL")
            if str(connection.execute("PRAGMA journal_mode = WAL").fetchone()[0]).lower() != "wal":
                raise StoreError("SQLite WAL mode is required")
            connection.execute("BEGIN IMMEDIATE")
            for _, sql in _SCHEMA_OBJECTS:
                connection.execute(sql)
            root_document = capability_root.document()
            connection.execute(
                """INSERT INTO authoring_capability_root(
                       singleton, workspace_id, authority_paths_hash, human_review_capability_hash,
                       replay_runner_capability_hash, publication_capability_hash, replay_policy_hash
                   ) VALUES(1, ?, ?, ?, ?, ?, ?)""",
                (
                    root_document["workspace_id"],
                    authority_paths_digest,
                    root_document["human_review_capability_hash"],
                    root_document["replay_runner_capability_hash"],
                    root_document["publication_capability_hash"],
                    root_document["replay_policy_hash"],
                ),
            )
            connection.execute(
                "INSERT INTO published_head(singleton, publication_id, generation) VALUES(1, NULL, 0)"
            )
            connection.execute(f"PRAGMA application_id = {APPLICATION_ID}")
            connection.execute(f"PRAGMA user_version = {SCHEMA_VERSION}")
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()
        require_plain_file(database)
        return cls(workspace)

    def _recheck_root(self) -> None:
        require_plain_ancestors(self.root)
        if identity(require_plain_directory(self.root)) != self._root_identity:
            raise StoreError("authoring workspace identity changed")

    @contextlib.contextmanager
    def exclusive(self) -> Iterator[None]:
        """One re-entrant process-local and cross-process workspace lock."""

        local = _local_lock(self.lock_file)
        with local:
            depth = getattr(self._lock_state, "depth", 0)
            if depth:
                self._lock_state.depth = depth + 1
                try:
                    yield
                finally:
                    self._lock_state.depth -= 1
                return
            self._recheck_root()
            before = require_plain_file(self.lock_file)
            flags = os.O_RDWR | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0)
            descriptor = os.open(self.lock_file, flags)
            try:
                opened = os.fstat(descriptor)
                if identity(before) != identity(opened):
                    raise StoreError("workspace lock identity changed")
                if os.name == "nt":
                    import msvcrt

                    os.lseek(descriptor, 0, os.SEEK_SET)
                    msvcrt.locking(descriptor, msvcrt.LK_LOCK, 1)
                else:
                    import fcntl

                    fcntl.flock(descriptor, fcntl.LOCK_EX)
                if identity(opened) != identity(self.lock_file.lstat()):
                    raise StoreError("workspace lock path changed")
                self._lock_state.depth = 1
                try:
                    yield
                finally:
                    self._lock_state.depth = 0
            finally:
                try:
                    if os.name == "nt":
                        import msvcrt

                        os.lseek(descriptor, 0, os.SEEK_SET)
                        msvcrt.locking(descriptor, msvcrt.LK_UNLCK, 1)
                    else:
                        import fcntl

                        fcntl.flock(descriptor, fcntl.LOCK_UN)
                finally:
                    os.close(descriptor)

    def _connect(self) -> sqlite3.Connection:
        self._recheck_root()
        before = require_plain_file(self.database)
        connection = sqlite3.connect(self.database, timeout=5.0)
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        connection.execute("PRAGMA busy_timeout = 5000")
        connection.execute("PRAGMA synchronous = FULL")
        connection.execute("PRAGMA trusted_schema = OFF")
        after = require_plain_file(self.database)
        if identity(before) != identity(after):
            connection.close()
            raise StoreError("authoring database identity changed while opening")
        for suffix in ("-wal", "-shm"):
            sidecar = Path(str(self.database) + suffix)
            if os.path.lexists(sidecar):
                require_plain_file(sidecar)
        if connection.execute("PRAGMA foreign_keys").fetchone()[0] != 1:
            connection.close()
            raise StoreError("SQLite foreign keys are required")
        if connection.execute("PRAGMA synchronous").fetchone()[0] != 2:
            connection.close()
            raise StoreError("SQLite synchronous=FULL is required")
        return connection

    @contextlib.contextmanager
    def _transaction(self) -> Iterator[sqlite3.Connection]:
        connection = self._connect()
        try:
            connection.execute("BEGIN IMMEDIATE")
            yield connection
            connection.commit()
        except BaseException:
            connection.rollback()
            raise
        finally:
            connection.close()

    def _validate_database(self) -> None:
        with contextlib.closing(self._connect()) as connection:
            if connection.execute("PRAGMA application_id").fetchone()[0] != APPLICATION_ID:
                raise StoreError("workspace has the wrong SQLite application_id")
            if connection.execute("PRAGMA user_version").fetchone()[0] != SCHEMA_VERSION:
                raise StoreError("workspace has the wrong schema version")
            if str(connection.execute("PRAGMA journal_mode").fetchone()[0]).lower() != "wal":
                raise StoreError("workspace is not in WAL mode")
            actual = {
                row["name"]: _normalize_sql(row["sql"])
                for row in connection.execute(
                    """SELECT name, sql FROM sqlite_schema
                       WHERE name NOT LIKE 'sqlite_%' AND sql IS NOT NULL"""
                )
            }
            expected = {name: _normalize_sql(sql) for name, sql in _SCHEMA_OBJECTS}
            if actual != expected:
                raise StoreError("workspace schema objects do not exactly match annotation v2")
            if connection.execute("PRAGMA quick_check").fetchone()[0] != "ok":
                raise StoreError("workspace SQLite quick_check failed")
            if connection.execute("SELECT count(*) FROM authoring_capability_root").fetchone()[0] != 1:
                raise StoreError("workspace authority root is missing or ambiguous")
            if connection.execute("SELECT count(*) FROM published_head").fetchone()[0] != 1:
                raise StoreError("workspace published head is missing or ambiguous")

    def capability_root(self) -> AuthoringCapabilityRoot:
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                """SELECT workspace_id, human_review_capability_hash,
                          replay_runner_capability_hash, publication_capability_hash,
                          replay_policy_hash
                   FROM authoring_capability_root WHERE singleton = 1"""
            ).fetchone()
        if row is None:
            raise StoreError("workspace authority root is missing")
        return AuthoringCapabilityRoot(**dict(row))

    def pinned_authority_paths_hash(self) -> str:
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                "SELECT authority_paths_hash FROM authoring_capability_root WHERE singleton = 1"
            ).fetchone()
        if row is None:
            raise StoreError("workspace authority root is missing")
        return _hash(row["authority_paths_hash"], "pinned authority paths hash")

    def _verify_capability(self, capability: Any, capability_type: type[Any], root_field: str) -> None:
        if type(capability) is not capability_type:
            raise StoreError("wrong trusted capability type for this operation")
        capability.verify_open()
        if capability.sha256 != getattr(self.capability_root(), root_field):
            raise StoreError("trusted capability is not pinned by this workspace")

    def verify_human_review_capability(self, capability: HumanReviewCapability) -> None:
        self._verify_capability(
            capability, HumanReviewCapability, "human_review_capability_hash"
        )

    def verify_replay_runner_capability(self, capability: ReplayRunnerCapability) -> None:
        self._verify_capability(
            capability, ReplayRunnerCapability, "replay_runner_capability_hash"
        )

    def verify_publication_capability(self, capability: PublicationCapability) -> None:
        self._verify_capability(
            capability, PublicationCapability, "publication_capability_hash"
        )

    def verify_replay_policy(self, policy: ReplayPolicy) -> None:
        if type(policy) is not ReplayPolicy:
            raise StoreError("replay policy must come from its trusted descriptor opener")
        policy.verify_open()
        if policy.exact_hash != self.capability_root().replay_policy_hash:
            raise StoreError("replay policy is not pinned by this workspace")

    @staticmethod
    def _validate_asset_content(content: bytes, asset_type: str) -> None:
        if not content or len(content) > _MAX_RUNTIME_ASSET_BYTES:
            raise StoreError("runtime asset size is outside the RuntimeArtifact schema ceiling")
        if asset_type == "template_png":
            if (
                len(content) < 24
                or content[:8] != b"\x89PNG\r\n\x1a\n"
                or content[12:16] != b"IHDR"
                or int.from_bytes(content[16:20], "big") == 0
                or int.from_bytes(content[20:24], "big") == 0
            ):
                raise StoreError("template_png is not a non-empty PNG image")
        elif asset_type == "template_webp":
            if (
                len(content) < 12
                or content[:4] != b"RIFF"
                or content[8:12] != b"WEBP"
                or int.from_bytes(content[4:8], "little") + 8 != len(content)
            ):
                raise StoreError("template_webp is not an exact RIFF WebP image")
        else:
            raise StoreError(f"unsupported deployable asset type {asset_type!r}")

    def _blob_path(self, digest: str, kind: str, asset_type: str | None) -> Path:
        if kind == "evidence" and asset_type is None:
            return self.evidence_blobs / digest[:2] / digest
        if kind == "runtime_asset" and asset_type in _ASSET_TYPES:
            return self.runtime_blobs / str(asset_type) / digest[:2] / digest
        raise StoreError("blob kind and asset type do not form an allowed namespace")

    def _verify_blob_file(self, reference: BlobRef) -> bytes:
        path = self._blob_path(reference.sha256, reference.kind, reference.asset_type)
        try:
            content = read_plain_file(path, maximum=_MAX_RUNTIME_ASSET_BYTES)
        except UnsafePath as error:
            raise StoreError(f"immutable blob {reference.sha256} is unavailable") from error
        if len(content) != reference.size or _sha256(content) != reference.sha256:
            raise StoreError(f"immutable blob {reference.sha256} is corrupt")
        if reference.kind == "runtime_asset":
            self._validate_asset_content(content, str(reference.asset_type))
        return content

    def _materialize_blob(self, upload: BlobUpload) -> tuple[BlobRef, bool]:
        if not isinstance(upload.content, bytes):
            raise StoreError("blob content must be bytes")
        if upload.kind == "evidence":
            if upload.asset_type is not None:
                raise StoreError("evidence cannot carry a deployable asset type")
        elif upload.kind == "runtime_asset":
            if upload.asset_type not in _ASSET_TYPES:
                raise StoreError("runtime asset requires one explicit deployable asset type")
            self._validate_asset_content(upload.content, str(upload.asset_type))
        else:
            raise StoreError(f"unknown blob kind {upload.kind!r}")
        digest = _sha256(upload.content)
        reference = BlobRef(digest, len(upload.content), upload.kind, upload.asset_type)
        destination = self._blob_path(digest, upload.kind, upload.asset_type)
        make_plain_directories(destination.parent)
        if os.path.lexists(destination):
            self._verify_blob_file(reference)
            return reference, False
        try:
            write_new_file(destination, upload.content)
        except (OSError, UnsafePath) as error:
            if os.path.lexists(destination):
                self._verify_blob_file(reference)
                return reference, False
            raise StoreError("blob file could not be materialized") from error
        self._verify_blob_file(reference)
        return reference, True

    @staticmethod
    def _insert_blob_row(connection: sqlite3.Connection, reference: BlobRef) -> None:
        existing = connection.execute(
            "SELECT size, kind, asset_type FROM blobs WHERE sha256 = ?", (reference.sha256,)
        ).fetchone()
        if existing is not None:
            if (
                existing["size"] != reference.size
                or existing["kind"] != reference.kind
                or existing["asset_type"] != reference.asset_type
            ):
                raise Conflict(
                    f"blob {reference.sha256} already belongs to another immutable namespace"
                )
            return
        total = connection.execute("SELECT coalesce(sum(size), 0) FROM blobs").fetchone()[0]
        if total + reference.size > _MAX_BLOB_STORE_BYTES:
            raise StoreError("authoring blob quota is exhausted")
        connection.execute(
            "INSERT INTO blobs(sha256, size, kind, asset_type, created_at) VALUES(?, ?, ?, ?, ?)",
            (
                reference.sha256,
                reference.size,
                reference.kind,
                reference.asset_type,
                _now(),
            ),
        )

    def _cleanup_created(self, created: Sequence[BlobRef]) -> None:
        for reference in created:
            with contextlib.closing(self._connect()) as connection:
                row = connection.execute(
                    "SELECT size, kind, asset_type FROM blobs WHERE sha256 = ?",
                    (reference.sha256,),
                ).fetchone()
            exact_row_exists = row is not None and (
                row["size"] == reference.size
                and row["kind"] == reference.kind
                and row["asset_type"] == reference.asset_type
            )
            if not exact_row_exists:
                path = self._blob_path(reference.sha256, reference.kind, reference.asset_type)
                if os.path.lexists(path):
                    try:
                        content = read_plain_file(path, maximum=_MAX_RUNTIME_ASSET_BYTES)
                        if _sha256(content) != reference.sha256:
                            raise StoreError("refusing to clean a changed failed upload")
                        path.unlink()
                    except UnsafePath as error:
                        raise StoreError("failed upload cleanup encountered an unsafe path") from error

    def put_evidence_blob(self, content: bytes) -> BlobRef:
        return self._put_blob(BlobUpload("evidence", content))

    def put_runtime_asset(self, content: bytes, asset_type: str) -> BlobRef:
        return self._put_blob(BlobUpload("runtime_asset", content, asset_type))

    def _put_blob(self, upload: BlobUpload) -> BlobRef:
        with self.exclusive():
            reference, created = self._materialize_blob(upload)
            try:
                with self._transaction() as connection:
                    self._insert_blob_row(connection, reference)
            except BaseException:
                if created:
                    self._cleanup_created([reference])
                raise
            return reference

    def read_blob(
        self,
        digest: str,
        required_kind: str,
        required_asset_type: str | None = None,
    ) -> bytes:
        _hash(digest, "blob hash")
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                "SELECT size, kind, asset_type FROM blobs WHERE sha256 = ?", (digest,)
            ).fetchone()
        if row is None or row["kind"] != required_kind:
            raise NotFound(f"{required_kind} blob {digest} was not found")
        if required_kind == "runtime_asset" and row["asset_type"] != required_asset_type:
            raise Conflict("runtime asset type does not match its immutable blob identity")
        return self._verify_blob_file(
            BlobRef(digest, row["size"], row["kind"], row["asset_type"])
        )

    @staticmethod
    def _candidate(value: Mapping[str, Any]) -> tuple[dict[str, Any], str, str]:
        encoded = _canonical_document(dict(value))
        document = _object(encoded)
        errors = validate_candidate_model(document)
        if errors:
            first = errors[0]
            raise StoreError(f"invalid candidate at {first['path']}: {first['message']}")
        return document, encoded, _sha256(encoded.encode("utf-8"))

    @staticmethod
    def _require_candidate_blobs(connection: sqlite3.Connection, candidate: Mapping[str, Any]) -> None:
        for digest in candidate["evidence_blob_hashes"]:
            row = connection.execute(
                "SELECT kind, asset_type FROM blobs WHERE sha256 = ?", (digest,)
            ).fetchone()
            if row is None or row["kind"] != "evidence" or row["asset_type"] is not None:
                raise NotFound(f"evidence blob {digest} was not found")
        for asset in candidate["runtime_assets"]:
            row = connection.execute(
                "SELECT kind, asset_type FROM blobs WHERE sha256 = ?", (asset["sha256"],)
            ).fetchone()
            if (
                row is None
                or row["kind"] != "runtime_asset"
                or row["asset_type"] != asset["asset_type"]
            ):
                raise NotFound(f"runtime asset blob {asset['sha256']} was not found with its type")

    @staticmethod
    def _insert_candidate_refs(connection: sqlite3.Connection, candidate: Mapping[str, Any]) -> None:
        for digest in candidate["evidence_blob_hashes"]:
            connection.execute(
                """INSERT INTO candidate_blob_refs(
                       candidate_id, candidate_revision, sha256, role, asset_path
                   ) VALUES(?, ?, ?, 'evidence', NULL)""",
                (candidate["id"], candidate["revision"], digest),
            )
        for asset in candidate["runtime_assets"]:
            connection.execute(
                """INSERT INTO candidate_blob_refs(
                       candidate_id, candidate_revision, sha256, role, asset_path
                   ) VALUES(?, ?, ?, 'runtime_asset', ?)""",
                (candidate["id"], candidate["revision"], asset["sha256"], asset["path"]),
            )

    def _store_candidate(
        self,
        connection: sqlite3.Connection,
        document: Mapping[str, Any],
        encoded: str,
        document_hash: str,
        expected_revision: int | None,
    ) -> None:
        self._require_candidate_blobs(connection, document)
        if expected_revision is None:
            if document["revision"] != 1:
                raise StoreError("a new candidate must start at revision 1")
            if connection.execute(
                "SELECT 1 FROM candidate_heads WHERE candidate_id = ?", (document["id"],)
            ).fetchone() is not None:
                raise Conflict(f"candidate {document['id']!r} already exists")
        else:
            _positive_integer(expected_revision, "expected_revision")
            if document["revision"] != expected_revision + 1:
                raise StoreError("candidate revision must advance exactly once")
            head = connection.execute(
                "SELECT revision FROM candidate_heads WHERE candidate_id = ?", (document["id"],)
            ).fetchone()
            if head is None:
                raise NotFound(f"candidate {document['id']!r} was not found")
            if head["revision"] != expected_revision:
                raise Conflict("candidate head compare-and-swap failed")
        _require_document_budget(connection, encoded)
        connection.execute(
            """INSERT INTO candidate_revisions(
                   candidate_id, revision, document, document_hash, created_at
               ) VALUES(?, ?, ?, ?, ?)""",
            (document["id"], document["revision"], encoded, document_hash, _now()),
        )
        self._insert_candidate_refs(connection, document)
        if expected_revision is None:
            connection.execute(
                "INSERT INTO candidate_heads(candidate_id, revision) VALUES(?, 1)",
                (document["id"],),
            )
        else:
            changed = connection.execute(
                """UPDATE candidate_heads SET revision = ?
                   WHERE candidate_id = ? AND revision = ?""",
                (document["revision"], document["id"], expected_revision),
            ).rowcount
            if changed != 1:
                raise Conflict("candidate head compare-and-swap failed")

    def commit_candidate_with_uploads(
        self,
        candidate: Mapping[str, Any],
        expected_revision: int | None,
        uploads: Sequence[BlobUpload],
    ) -> dict[str, Any]:
        document, encoded, document_hash = self._candidate(candidate)
        referenced = {("evidence", None, value) for value in document["evidence_blob_hashes"]}
        referenced.update(
            ("runtime_asset", value["asset_type"], value["sha256"])
            for value in document["runtime_assets"]
        )
        prepared: list[BlobRef] = []
        for upload in uploads:
            digest = _sha256(upload.content)
            key = (upload.kind, upload.asset_type, digest)
            if key not in referenced:
                raise StoreError("candidate upload contains an unreferenced or reclassified blob")
            prepared.append(BlobRef(digest, len(upload.content), upload.kind, upload.asset_type))
        if len({item.sha256 for item in prepared}) != len(prepared):
            raise StoreError("candidate uploads must have unique byte identities")
        with self.exclusive():
            materialized: list[BlobRef] = []
            created: list[BlobRef] = []
            try:
                for upload in uploads:
                    reference, was_created = self._materialize_blob(upload)
                    materialized.append(reference)
                    if was_created:
                        created.append(reference)
                with self._transaction() as connection:
                    for reference in materialized:
                        self._insert_blob_row(connection, reference)
                    self._store_candidate(
                        connection, document, encoded, document_hash, expected_revision
                    )
            except BaseException:
                self._cleanup_created(created)
                raise
        return document

    def create_candidate(self, candidate: Mapping[str, Any]) -> dict[str, Any]:
        return self.commit_candidate_with_uploads(candidate, None, ())

    def commit_candidate(self, candidate: Mapping[str, Any], expected_revision: int) -> dict[str, Any]:
        return self.commit_candidate_with_uploads(candidate, expected_revision, ())

    def candidates(self) -> list[dict[str, Any]]:
        with contextlib.closing(self._connect()) as connection:
            rows = connection.execute(
                """SELECT revisions.document FROM candidate_heads AS heads
                   JOIN candidate_revisions AS revisions
                     ON revisions.candidate_id = heads.candidate_id
                    AND revisions.revision = heads.revision
                   ORDER BY heads.candidate_id"""
            ).fetchall()
        return [_object(row["document"]) for row in rows]

    def get_candidate(self, candidate_id: str, revision: int | None = None) -> dict[str, Any]:
        with contextlib.closing(self._connect()) as connection:
            if revision is None:
                row = connection.execute(
                    """SELECT revisions.document FROM candidate_heads AS heads
                       JOIN candidate_revisions AS revisions
                         ON revisions.candidate_id = heads.candidate_id
                        AND revisions.revision = heads.revision
                       WHERE heads.candidate_id = ?""",
                    (candidate_id,),
                ).fetchone()
            else:
                _positive_integer(revision, "revision")
                row = connection.execute(
                    "SELECT document FROM candidate_revisions WHERE candidate_id = ? AND revision = ?",
                    (candidate_id, revision),
                ).fetchone()
        if row is None:
            raise NotFound(f"candidate {candidate_id!r} revision {revision!r} was not found")
        return _object(row["document"])

    @staticmethod
    def _checkpoint(value: Mapping[str, Any]) -> tuple[dict[str, Any], str, str]:
        required = {
            "candidate",
            "evidence_blob_hashes",
            "input_artifact_roots",
            "job_id",
            "resume_state",
            "revision",
            "phase",
        }
        if set(value) != required:
            raise StoreError(f"checkpoint fields must be exactly {sorted(required)!r}")
        document = _object(_canonical_document(dict(value)))
        if not isinstance(document["job_id"], str) or not document["job_id"]:
            raise StoreError("checkpoint job_id must be a non-empty string")
        _positive_integer(document["revision"], "checkpoint revision")
        if document["phase"] not in _CHECKPOINT_STAGES:
            raise StoreError("checkpoint phase is invalid")
        if not isinstance(document["resume_state"], dict):
            raise StoreError("checkpoint resume_state must be an object")
        for name in ("input_artifact_roots", "evidence_blob_hashes"):
            values = document[name]
            if (
                not isinstance(values, list)
                or len(values) != len(set(values))
                or any(not isinstance(item, str) or _SHA256.fullmatch(item) is None for item in values)
            ):
                raise StoreError(f"checkpoint {name} must contain unique SHA-256 values")
        candidate = document["candidate"]
        if candidate is not None:
            if not isinstance(candidate, dict) or set(candidate) != {"id", "revision"}:
                raise StoreError("checkpoint candidate must be null or an exact candidate reference")
            if not isinstance(candidate["id"], str) or not candidate["id"]:
                raise StoreError("checkpoint candidate id must be non-empty")
            _positive_integer(candidate["revision"], "checkpoint candidate revision")
        encoded = _canonical_document(document)
        return document, encoded, _sha256(encoded.encode("utf-8"))

    def save_agent_checkpoint_with_uploads(
        self,
        checkpoint: Mapping[str, Any],
        expected_revision: int | None,
        uploads: Sequence[BlobUpload],
    ) -> dict[str, Any]:
        document, encoded, document_hash = self._checkpoint(checkpoint)
        references = set(document["evidence_blob_hashes"])
        if any(upload.kind != "evidence" or upload.asset_type is not None for upload in uploads):
            raise StoreError("checkpoint uploads can only contain evidence")
        if any(_sha256(upload.content) not in references for upload in uploads):
            raise StoreError("checkpoint upload contains unreferenced evidence")
        if len({_sha256(upload.content) for upload in uploads}) != len(uploads):
            raise StoreError("checkpoint uploads must have unique byte identities")
        revision = document["revision"]
        if expected_revision is None and revision != 1:
            raise StoreError("a new checkpoint must start at revision 1")
        if expected_revision is not None:
            _positive_integer(expected_revision, "expected checkpoint revision")
            if revision != expected_revision + 1:
                raise StoreError("checkpoint revision must advance exactly once")
        with self.exclusive():
            materialized: list[BlobRef] = []
            created: list[BlobRef] = []
            try:
                for upload in uploads:
                    reference, was_created = self._materialize_blob(upload)
                    materialized.append(reference)
                    if was_created:
                        created.append(reference)
                with self._transaction() as connection:
                    for reference in materialized:
                        self._insert_blob_row(connection, reference)
                    candidate = document["candidate"]
                    candidate_id = None if candidate is None else candidate["id"]
                    candidate_revision = None if candidate is None else candidate["revision"]
                    if candidate is not None and connection.execute(
                        """SELECT 1 FROM candidate_revisions
                           WHERE candidate_id = ? AND revision = ?""",
                        (candidate_id, candidate_revision),
                    ).fetchone() is None:
                        raise NotFound("checkpoint candidate revision was not found")
                    for digest in references:
                        row = connection.execute(
                            "SELECT kind, asset_type FROM blobs WHERE sha256 = ?", (digest,)
                        ).fetchone()
                        if row is None or row["kind"] != "evidence" or row["asset_type"] is not None:
                            raise NotFound(f"checkpoint evidence blob {digest} was not found")
                    _require_document_budget(connection, encoded)
                    if expected_revision is None:
                        try:
                            connection.execute(
                                """INSERT INTO agent_checkpoints(
                                       job_id, revision, candidate_id, candidate_revision,
                                       document, document_hash, updated_at
                                   ) VALUES(?, ?, ?, ?, ?, ?, ?)""",
                                (
                                    document["job_id"],
                                    revision,
                                    candidate_id,
                                    candidate_revision,
                                    encoded,
                                    document_hash,
                                    _now(),
                                ),
                            )
                        except sqlite3.IntegrityError as error:
                            raise Conflict(f"checkpoint {document['job_id']!r} already exists") from error
                    else:
                        changed = connection.execute(
                            """UPDATE agent_checkpoints
                               SET revision = ?, candidate_id = ?, candidate_revision = ?,
                                   document = ?, document_hash = ?, updated_at = ?
                               WHERE job_id = ? AND revision = ?""",
                            (
                                revision,
                                candidate_id,
                                candidate_revision,
                                encoded,
                                document_hash,
                                _now(),
                                document["job_id"],
                                expected_revision,
                            ),
                        ).rowcount
                        if changed != 1:
                            raise Conflict("agent checkpoint compare-and-swap failed")
                        connection.execute(
                            "DELETE FROM agent_checkpoint_blob_refs WHERE job_id = ?",
                            (document["job_id"],),
                        )
                    for digest in sorted(references):
                        connection.execute(
                            "INSERT INTO agent_checkpoint_blob_refs(job_id, sha256) VALUES(?, ?)",
                            (document["job_id"], digest),
                        )
            except BaseException:
                self._cleanup_created(created)
                raise
        return document

    def save_agent_checkpoint(
        self, checkpoint: Mapping[str, Any], expected_revision: int | None
    ) -> dict[str, Any]:
        return self.save_agent_checkpoint_with_uploads(checkpoint, expected_revision, ())

    def get_agent_checkpoint(self, job_id: str) -> dict[str, Any]:
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                "SELECT document FROM agent_checkpoints WHERE job_id = ?", (job_id,)
            ).fetchone()
        if row is None:
            raise NotFound(f"agent checkpoint {job_id!r} was not found")
        return _object(row["document"])

    def review_candidate(
        self,
        capability: HumanReviewCapability,
        candidate_id: str,
        candidate_revision: int,
        outcome: str,
        comment: str,
    ) -> dict[str, Any]:
        self.verify_human_review_capability(capability)
        _positive_integer(candidate_revision, "candidate revision")
        if outcome not in {"accepted", "rejected"} or not isinstance(comment, str):
            raise StoreError("review requires accepted/rejected and a string comment")
        candidate = self.get_candidate(candidate_id, candidate_revision)
        if outcome == "accepted" and candidate["open_issues"]:
            raise Conflict("a candidate with open issues cannot be accepted")
        identity_document = {
            "candidate_id": candidate_id,
            "candidate_revision": candidate_revision,
            "capability_hash": capability.sha256,
            "comment": comment,
            "outcome": outcome,
            "reviewer_principal": capability.principal,
        }
        decision_id = _sha256(jcs_bytes(identity_document))
        with self.exclusive(), self._transaction() as connection:
            self.verify_human_review_capability(capability)
            existing = connection.execute(
                """SELECT decision_id, reviewer_principal, capability_hash, outcome, comment, created_at
                   FROM review_decisions WHERE candidate_id = ? AND candidate_revision = ?""",
                (candidate_id, candidate_revision),
            ).fetchone()
            if existing is not None:
                if existing["decision_id"] != decision_id:
                    raise Conflict("candidate revision already has a different immutable review")
                return {
                    **dict(existing),
                    "candidate_id": candidate_id,
                    "candidate_revision": candidate_revision,
                }
            created_at = _now()
            connection.execute(
                """INSERT INTO review_decisions(
                       decision_id, candidate_id, candidate_revision, reviewer_principal,
                       capability_hash, outcome, comment, created_at
                   ) VALUES(?, ?, ?, ?, ?, ?, ?, ?)""",
                (
                    decision_id,
                    candidate_id,
                    candidate_revision,
                    capability.principal,
                    capability.sha256,
                    outcome,
                    comment,
                    created_at,
                ),
            )
        return {**identity_document, "decision_id": decision_id, "created_at": created_at}

    def review_for(self, candidate_id: str, candidate_revision: int) -> dict[str, Any] | None:
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                """SELECT decision_id, candidate_id, candidate_revision, reviewer_principal,
                          capability_hash, outcome, comment, created_at
                   FROM review_decisions WHERE candidate_id = ? AND candidate_revision = ?""",
                (candidate_id, candidate_revision),
            ).fetchone()
        return None if row is None else dict(row)

    @staticmethod
    def _validate_replay_result(value: Mapping[str, Any]) -> dict[str, Any]:
        fields = {
            "candidate_id",
            "candidate_revision",
            "runtime_model_hash",
            "kind",
            "corpus_hash",
            "passed",
            "report",
        }
        if set(value) != fields:
            raise StoreError(f"trusted replay result fields must be exactly {sorted(fields)!r}")
        result = _object(_canonical_document(dict(value)))
        if not isinstance(result["candidate_id"], str) or not result["candidate_id"]:
            raise StoreError("trusted replay candidate_id is required")
        _positive_integer(result["candidate_revision"], "trusted replay candidate revision")
        _hash(result["runtime_model_hash"], "trusted replay RuntimeModel hash")
        if result["kind"] not in {"frame", "transition"}:
            raise StoreError("trusted replay kind must be frame or transition")
        _hash(result["corpus_hash"], "trusted replay corpus hash")
        if result["passed"] is not True or not isinstance(result["report"], dict):
            raise Conflict("only a passing replay with an object report can be attested")
        return result

    def record_trusted_replay_result(
        self,
        replay_capability: ReplayRunnerCapability,
        replay_policy: ReplayPolicy,
        result: Mapping[str, Any],
    ) -> dict[str, Any]:
        """Trusted runner fixes all replay identity before publication can see it."""

        self.verify_replay_runner_capability(replay_capability)
        self.verify_replay_policy(replay_policy)
        document = self._validate_replay_result(result)
        if document["corpus_hash"] != replay_policy.corpus_for(document["kind"]):
            raise Conflict("trusted replay did not use the pinned corpus")
        candidate = self.get_candidate(document["candidate_id"], document["candidate_revision"])
        current = self.get_candidate(document["candidate_id"])
        if current["revision"] != document["candidate_revision"]:
            raise Conflict("trusted replay can only attest the current candidate head")
        try:
            _, compiled_hash = compile_runtime_toml(build_runtime_model(candidate))
        except SchemaIssue as error:
            raise StoreError(str(error)) from error
        if compiled_hash != document["runtime_model_hash"]:
            raise Conflict("trusted replay result is bound to another RuntimeModel")
        report = _canonical_document(document["report"])
        report_hash = _sha256(report.encode("utf-8"))
        identity_document = {
            "candidate_id": document["candidate_id"],
            "candidate_revision": document["candidate_revision"],
            "capability_hash": replay_capability.sha256,
            "corpus_hash": document["corpus_hash"],
            "kind": document["kind"],
            "passed": True,
            "replay_policy_hash": replay_policy.exact_hash,
            "report_hash": report_hash,
            "runner_principal": replay_capability.principal,
            "runtime_model_hash": document["runtime_model_hash"],
        }
        result_id = _sha256(jcs_bytes(identity_document))
        with self.exclusive(), self._transaction() as connection:
            self.verify_replay_runner_capability(replay_capability)
            self.verify_replay_policy(replay_policy)
            head = connection.execute(
                "SELECT revision FROM candidate_heads WHERE candidate_id = ?",
                (document["candidate_id"],),
            ).fetchone()
            if head is None or head["revision"] != document["candidate_revision"]:
                raise Conflict("candidate changed before trusted replay result was recorded")
            try:
                connection.execute(
                    """INSERT INTO replay_result_intents(
                           result_id, candidate_id, candidate_revision, runtime_model_hash,
                           kind, corpus_hash, replay_policy_hash, passed, report_hash, report,
                           runner_principal, capability_hash, created_at
                       ) VALUES(?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?)""",
                    (
                        result_id,
                        document["candidate_id"],
                        document["candidate_revision"],
                        document["runtime_model_hash"],
                        document["kind"],
                        document["corpus_hash"],
                        replay_policy.exact_hash,
                        report_hash,
                        report,
                        replay_capability.principal,
                        replay_capability.sha256,
                        _now(),
                    ),
                )
            except sqlite3.IntegrityError:
                existing = connection.execute(
                    "SELECT * FROM replay_result_intents WHERE result_id = ?", (result_id,)
                ).fetchone()
                if existing is None:
                    raise Conflict("a different replay result already owns this identity")
        return self.replay_result(result_id, include_report=True)

    def replay_result(self, result_id: str, *, include_report: bool = False) -> dict[str, Any]:
        _hash(result_id, "replay result id")
        columns = "*" if include_report else "result_id, candidate_id, candidate_revision, runtime_model_hash, kind, corpus_hash, replay_policy_hash, passed, report_hash, runner_principal, capability_hash, created_at"
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                f"SELECT {columns} FROM replay_result_intents WHERE result_id = ?", (result_id,)
            ).fetchone()
        if row is None:
            raise NotFound(f"trusted replay result {result_id} was not found")
        document = dict(row)
        document["passed"] = bool(document["passed"])
        if include_report:
            document["report"] = _object(document["report"])
        return document

    def _bundle_path(self, bundle_id: str) -> Path:
        return self.replay_bundles / bundle_id[:2] / bundle_id

    @staticmethod
    def _bundle_identity(document: Mapping[str, Any]) -> str:
        """The bundle id is the content address of its closure, minus the id itself."""

        return _sha256(jcs_bytes({key: value for key, value in document.items() if key != "bundle_id"}))

    @classmethod
    def _bundle_document(cls, value: Mapping[str, Any]) -> dict[str, Any]:
        """Mint the identity, then validate the whole closure.

        Shape, field set, bounds and the frames/retention pairing are the
        checked-in contract's job and are checked by the official validator
        against it. Only what a JSON Schema cannot say is checked here: that the
        Journal prefix follows the baseline rather than repeating it, that no
        list repeats an entry, and that a retained frame window is real.
        """

        if "bundle_id" in value:
            raise StoreError("replay bundle identity is minted here, not supplied")
        content = _object(_canonical_document(dict(value)))
        document = {**content, "bundle_id": cls._bundle_identity(content)}
        try:
            require_valid(_ANNOTATION_SCHEMA, document, "replay bundle")
        except ValueError as error:
            raise StoreError(str(error)) from error
        for name in ("journal_prefix", "operation_rows", "observations", "frames"):
            if len(set(document[name])) != len(document[name]):
                raise StoreError(f"replay bundle {name} repeats one entry")
        if document["baseline_event_id"] in document["journal_prefix"]:
            raise StoreError("replay bundle journal_prefix must follow its baseline, not repeat it")
        if document["frames"]:
            deadline = _timestamp(
                document["frame_retention_expires_at"], "replay bundle frame retention"
            )
            now = _now_instant()
            if deadline <= now:
                raise StoreError("replay bundle frame retention has already expired")
            if deadline - now > _datetime.timedelta(seconds=_MAX_FRAME_RETENTION_SECONDS):
                raise StoreError("replay bundle frame retention exceeds the workspace ceiling")
        return document

    def _verify_bundle_file(self, bundle_id: str, row_document: str) -> dict[str, Any]:
        path = self._bundle_path(bundle_id)
        try:
            content = read_plain_file(path, maximum=_MAX_DOCUMENT_BYTES)
        except UnsafePath as error:
            raise StoreError(f"replay bundle {bundle_id} is unavailable") from error
        if content != row_document.encode("utf-8"):
            raise StoreError(f"replay bundle {bundle_id} file does not match its immutable row")
        try:
            document = load_exact_jcs(content)
        except CanonicalJsonError as error:
            raise StoreError(f"replay bundle {bundle_id} is not exact JCS") from error
        if not isinstance(document, dict) or document.get("bundle_id") != bundle_id:
            raise StoreError(f"replay bundle {bundle_id} does not name itself")
        if self._bundle_identity(document) != bundle_id:
            raise StoreError(f"replay bundle {bundle_id} has a false content address")
        return document

    def record_replay_bundle(
        self,
        replay_capability: ReplayRunnerCapability,
        bundle: Mapping[str, Any],
    ) -> dict[str, Any]:
        """Assemble one offline replay closure the trusted runner may later replay.

        The runner supplies the closure; the identity is minted here, so a
        bundle cannot claim a content address it does not have.
        """

        self.verify_replay_runner_capability(replay_capability)
        document = self._bundle_document(bundle)
        bundle_id = document["bundle_id"]
        encoded = jcs_bytes(document)
        with self.exclusive():
            self.verify_replay_runner_capability(replay_capability)
            path = self._bundle_path(bundle_id)
            make_plain_directories(path.parent)
            if os.path.lexists(path):
                try:
                    if read_plain_file(path, maximum=_MAX_DOCUMENT_BYTES) != encoded:
                        raise Conflict(f"replay bundle {bundle_id} already holds other bytes")
                except UnsafePath as error:
                    raise StoreError(f"replay bundle {bundle_id} is unavailable") from error
            else:
                try:
                    write_new_file(path, encoded)
                except (OSError, UnsafePath) as error:
                    raise StoreError("replay bundle file could not be materialized") from error
            with self._transaction() as connection:
                if connection.execute(
                    "SELECT 1 FROM replay_bundles WHERE bundle_id = ?", (bundle_id,)
                ).fetchone() is None:
                    _require_document_budget(connection, encoded.decode("utf-8"))
                    for role, digests in (
                        ("observation", document["observations"]),
                        ("frame", document["frames"]),
                    ):
                        for digest in digests:
                            row = connection.execute(
                                "SELECT kind, asset_type FROM blobs WHERE sha256 = ?", (digest,)
                            ).fetchone()
                            if row is None or row["kind"] != "evidence" or row["asset_type"] is not None:
                                raise NotFound(
                                    f"replay bundle {role} evidence blob {digest} was not found"
                                )
                    connection.execute(
                        """INSERT INTO replay_bundles(
                               bundle_id, baseline_event_id, session_manifest_hash,
                               journal_prefix_length, frame_count, frame_retention_expires_at,
                               document, runner_principal, capability_hash, created_at
                           ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                        (
                            bundle_id,
                            document["baseline_event_id"],
                            document["session_manifest_hash"],
                            len(document["journal_prefix"]),
                            len(document["frames"]),
                            document["frame_retention_expires_at"],
                            encoded.decode("utf-8"),
                            replay_capability.principal,
                            replay_capability.sha256,
                            _now(),
                        ),
                    )
                    for role, digests in (
                        ("observation", document["observations"]),
                        ("frame", document["frames"]),
                    ):
                        for digest in digests:
                            connection.execute(
                                """INSERT INTO replay_bundle_blob_refs(bundle_id, sha256, role)
                                   VALUES(?, ?, ?)""",
                                (bundle_id, digest, role),
                            )
        return self.replay_bundle(bundle_id)

    def replay_bundle(self, bundle_id: str) -> dict[str, Any]:
        """Read one bundle for audit. An expired frame window still reads."""

        _hash(bundle_id, "replay bundle id")
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                """SELECT bundle_id, baseline_event_id, session_manifest_hash,
                          journal_prefix_length, frame_count, frame_retention_expires_at,
                          document, runner_principal, capability_hash, created_at
                   FROM replay_bundles WHERE bundle_id = ?""",
                (bundle_id,),
            ).fetchone()
        if row is None:
            raise NotFound(f"replay bundle {bundle_id} was not found")
        return {**dict(row), "document": self._verify_bundle_file(bundle_id, row["document"])}

    def _require_publishable_bundle(
        self,
        connection: sqlite3.Connection,
        bundle_id: str,
    ) -> dict[str, Any]:
        row = connection.execute(
            "SELECT document, frame_retention_expires_at FROM replay_bundles WHERE bundle_id = ?",
            (bundle_id,),
        ).fetchone()
        if row is None:
            raise NotFound(f"replay bundle {bundle_id} was not found")
        document = self._verify_bundle_file(bundle_id, row["document"])
        expiry = row["frame_retention_expires_at"]
        if expiry is not None and _timestamp(expiry, "replay bundle frame retention") <= _now_instant():
            raise Conflict(f"replay bundle {bundle_id} frames are past their retention window")
        return document

    def record_project_operation_replay_result(
        self,
        replay_capability: ReplayRunnerCapability,
        replay_policy: ReplayPolicy,
        result: Mapping[str, Any],
    ) -> dict[str, Any]:
        """The project/operation gate's evidence: one bundle replayed by the trusted runner."""

        self.verify_replay_runner_capability(replay_capability)
        self.verify_replay_policy(replay_policy)
        fields = {"candidate_id", "candidate_revision", "replay_bundle_id", "passed", "report"}
        if set(result) != fields:
            raise StoreError(
                f"project/operation replay result fields must be exactly {sorted(fields)!r}"
            )
        document = _object(_canonical_document(dict(result)))
        if not isinstance(document["candidate_id"], str) or not document["candidate_id"]:
            raise StoreError("project/operation replay candidate_id is required")
        _positive_integer(document["candidate_revision"], "project/operation candidate revision")
        _hash(document["replay_bundle_id"], "project/operation replay bundle id")
        if document["passed"] is not True or not isinstance(document["report"], dict):
            raise Conflict(
                "only a passing project/operation replay with an object report can be attested"
            )
        current = self.get_candidate(document["candidate_id"])
        if current["revision"] != document["candidate_revision"]:
            raise Conflict("project/operation replay can only attest the current candidate head")
        report = _canonical_document(document["report"])
        report_hash = _sha256(report.encode("utf-8"))
        identity_document = {
            "candidate_id": document["candidate_id"],
            "candidate_revision": document["candidate_revision"],
            "capability_hash": replay_capability.sha256,
            "passed": True,
            "replay_bundle_id": document["replay_bundle_id"],
            "replay_policy_hash": replay_policy.exact_hash,
            "report_hash": report_hash,
            "runner_principal": replay_capability.principal,
        }
        result_id = _sha256(jcs_bytes(identity_document))
        with self.exclusive(), self._transaction() as connection:
            self.verify_replay_runner_capability(replay_capability)
            self.verify_replay_policy(replay_policy)
            self._require_publishable_bundle(connection, document["replay_bundle_id"])
            head = connection.execute(
                "SELECT revision FROM candidate_heads WHERE candidate_id = ?",
                (document["candidate_id"],),
            ).fetchone()
            if head is None or head["revision"] != document["candidate_revision"]:
                raise Conflict("candidate changed before the project/operation result was recorded")
            try:
                connection.execute(
                    """INSERT INTO project_operation_replay_intents(
                           result_id, replay_bundle_id, candidate_id, candidate_revision,
                           replay_policy_hash, passed, report_hash, report,
                           runner_principal, capability_hash, created_at
                       ) VALUES(?, ?, ?, ?, ?, 1, ?, ?, ?, ?, ?)""",
                    (
                        result_id,
                        document["replay_bundle_id"],
                        document["candidate_id"],
                        document["candidate_revision"],
                        replay_policy.exact_hash,
                        report_hash,
                        report,
                        replay_capability.principal,
                        replay_capability.sha256,
                        _now(),
                    ),
                )
            except sqlite3.IntegrityError:
                existing = connection.execute(
                    "SELECT 1 FROM project_operation_replay_intents WHERE result_id = ?",
                    (result_id,),
                ).fetchone()
                if existing is None:
                    raise Conflict(
                        "a different project/operation replay result already owns this identity"
                    )
        return self.project_operation_replay_result(result_id, include_report=True)

    def project_operation_replay_result(
        self,
        result_id: str,
        *,
        include_report: bool = False,
    ) -> dict[str, Any]:
        _hash(result_id, "project/operation replay result id")
        columns = (
            "*"
            if include_report
            else "result_id, replay_bundle_id, candidate_id, candidate_revision,"
            " replay_policy_hash, passed, report_hash, runner_principal, capability_hash, created_at"
        )
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                f"SELECT {columns} FROM project_operation_replay_intents WHERE result_id = ?",
                (result_id,),
            ).fetchone()
        if row is None:
            raise NotFound(f"project/operation replay result {result_id} was not found")
        document = dict(row)
        document["passed"] = bool(document["passed"])
        if include_report:
            document["report"] = _object(document["report"])
        return document

    def _project_operation_row(
        self,
        connection: sqlite3.Connection,
        candidate_id: str,
        candidate_revision: int,
        replay_policy: ReplayPolicy,
        result_id: Any,
        *,
        require_unconsumed: bool,
    ) -> sqlite3.Row:
        """Fail closed: an absent or ill-formed project/operation gate is not a warning."""

        if not isinstance(result_id, str) or _SHA256.fullmatch(result_id) is None:
            raise Conflict("publication requires one passing project/operation replay result id")
        row = connection.execute(
            "SELECT * FROM project_operation_replay_intents WHERE result_id = ?", (result_id,)
        ).fetchone()
        if row is None:
            raise NotFound(f"project/operation replay result {result_id} was not found")
        if (
            row["candidate_id"] != candidate_id
            or row["candidate_revision"] != candidate_revision
            or row["replay_policy_hash"] != replay_policy.exact_hash
            or row["passed"] != 1
        ):
            raise Conflict(
                "project/operation replay result identity does not match this publication"
            )
        if require_unconsumed and connection.execute(
            "SELECT 1 FROM project_operation_attestations WHERE replay_result_id = ?", (result_id,)
        ).fetchone() is not None:
            raise Conflict("project/operation replay result was already consumed")
        self._require_publishable_bundle(connection, row["replay_bundle_id"])
        return row

    @staticmethod
    def _gate_document(
        replay_policy: ReplayPolicy,
        ui_rows: Sequence[sqlite3.Row],
        project_row: sqlite3.Row,
    ) -> dict[str, Any]:
        """The checked-in two-gate shape: UI model replay and project/operation replay, inlined.

        Both are built here from their own table, so neither gate can ever be
        filled in from the other's evidence.
        """

        gate = {
            "project_operation_replay": {
                "attestation_id": project_row["result_id"],
                "passed": True,
                "replay_bundle_id": project_row["replay_bundle_id"],
            },
            "replay_policy_hash": replay_policy.exact_hash,
            "ui_model_replay": {
                "frame_attestation_id": ui_rows[0]["result_id"],
                "passed": True,
                "transition_attestation_id": ui_rows[1]["result_id"],
            },
        }
        try:
            require_valid(_ANNOTATION_SCHEMA, gate, "replay gate")
        except ValueError as error:
            raise StoreError(str(error)) from error
        return gate

    @staticmethod
    def _replay_rows(
        connection: sqlite3.Connection,
        candidate_id: str,
        candidate_revision: int,
        runtime_model_hash: str,
        replay_policy: ReplayPolicy,
        result_ids: Sequence[str],
        *,
        require_unconsumed: bool,
    ) -> list[sqlite3.Row]:
        if not isinstance(result_ids, (list, tuple)) or len(result_ids) != 2:
            raise Conflict("publication requires two distinct trusted replay result IDs")
        for result_id in result_ids:
            _hash(result_id, "replay result id")
        if result_ids[0] == result_ids[1]:
            raise Conflict("publication requires two distinct trusted replay result IDs")
        rows: list[sqlite3.Row] = []
        for result_id in result_ids:
            row = connection.execute(
                "SELECT * FROM replay_result_intents WHERE result_id = ?", (result_id,)
            ).fetchone()
            if row is None:
                raise NotFound(f"trusted replay result {result_id} was not found")
            if (
                row["candidate_id"] != candidate_id
                or row["candidate_revision"] != candidate_revision
                or row["runtime_model_hash"] != runtime_model_hash
                or row["replay_policy_hash"] != replay_policy.exact_hash
                or row["corpus_hash"] != replay_policy.corpus_for(row["kind"])
                or row["passed"] != 1
            ):
                raise Conflict("trusted replay result identity does not match this publication")
            if require_unconsumed and connection.execute(
                "SELECT 1 FROM replay_attestations WHERE replay_result_id = ?", (result_id,)
            ).fetchone() is not None:
                raise Conflict("trusted replay result was already consumed")
            rows.append(row)
        by_kind = {row["kind"]: row for row in rows}
        if set(by_kind) != {"frame", "transition"}:
            raise Conflict("publication requires exactly one frame and one transition replay")
        return [by_kind["frame"], by_kind["transition"]]

    def build_replay_gate(
        self,
        *,
        replay_policy: ReplayPolicy,
        candidate_id: str,
        candidate_revision: int,
        runtime_model_hash: str,
        replay_result_ids: Sequence[str],
        project_operation_result_id: Any,
    ) -> dict[str, Any]:
        self.verify_replay_policy(replay_policy)
        with contextlib.closing(self._connect()) as connection:
            rows = self._replay_rows(
                connection,
                candidate_id,
                candidate_revision,
                runtime_model_hash,
                replay_policy,
                replay_result_ids,
                require_unconsumed=True,
            )
            project = self._project_operation_row(
                connection,
                candidate_id,
                candidate_revision,
                replay_policy,
                project_operation_result_id,
                require_unconsumed=True,
            )
        return self._gate_document(replay_policy, rows, project)

    def publication_head(self) -> dict[str, Any]:
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                "SELECT publication_id, generation FROM published_head WHERE singleton = 1"
            ).fetchone()
        if row is None:
            raise StoreError("published head is missing")
        return dict(row)

    @staticmethod
    def _validate_release_manifest(
        release_manifest: Mapping[str, Any],
        *,
        candidate_id: str,
        candidate_revision: int,
        expected_predecessor: str | None,
        runtime_artifact_root_hash: str,
    ) -> bytes:
        fields = {
            "annotation_workspace_format",
            "candidate_id",
            "candidate_revision",
            "generation",
            "predecessor_publication_id",
            "replay_gate_hash",
            "runtime_artifact_root_hash",
            "workspace_sqlite_revision",
        }
        if set(release_manifest) != fields:
            raise StoreError("release manifest has the wrong exact v2 shape")
        for field in ("replay_gate_hash", "runtime_artifact_root_hash"):
            _hash(release_manifest[field], f"release {field}")
        _positive_integer(release_manifest["generation"], "release generation")
        _positive_integer(
            release_manifest["annotation_workspace_format"], "release annotation workspace format"
        )
        _positive_integer(
            release_manifest["workspace_sqlite_revision"], "release workspace SQLite revision"
        )
        if release_manifest["predecessor_publication_id"] is not None:
            _hash(release_manifest["predecessor_publication_id"], "release predecessor")
        if (
            release_manifest["annotation_workspace_format"] != ANNOTATION_WORKSPACE_FORMAT
            or release_manifest["workspace_sqlite_revision"] != SCHEMA_VERSION
            or release_manifest["candidate_id"] != candidate_id
            or release_manifest["candidate_revision"] != candidate_revision
            or release_manifest["predecessor_publication_id"] != expected_predecessor
            or release_manifest["runtime_artifact_root_hash"] != runtime_artifact_root_hash
        ):
            raise StoreError("release manifest authority inputs are inconsistent")
        return jcs_bytes(dict(release_manifest))

    def commit_publication(
        self,
        *,
        publication_capability: PublicationCapability,
        replay_policy: ReplayPolicy,
        candidate_id: str,
        candidate_revision: int,
        expected_predecessor: str | None,
        runtime_model_hash: str,
        runtime_artifact_root_hash: str,
        runtime_manifest_bytes: bytes,
        release_manifest: Mapping[str, Any],
        replay_result_ids: Sequence[str],
        project_operation_result_id: Any,
    ) -> dict[str, Any]:
        """Final short transaction: all CAS, both replay gates, publication, and head."""

        if getattr(self._lock_state, "depth", 0) <= 0:
            raise StoreError("publication commit requires the workspace cross-process lock")
        self.verify_publication_capability(publication_capability)
        self.verify_replay_policy(replay_policy)
        _positive_integer(candidate_revision, "candidate revision")
        _hash(runtime_model_hash, "RuntimeModel hash")
        _hash(runtime_artifact_root_hash, "RuntimeArtifact root hash")
        if expected_predecessor is not None:
            _hash(expected_predecessor, "expected predecessor")
        if _sha256(runtime_manifest_bytes) != runtime_artifact_root_hash:
            raise StoreError("RuntimeArtifact root does not match exact manifest bytes")
        try:
            runtime_manifest = load_exact_jcs(runtime_manifest_bytes)
        except CanonicalJsonError as error:
            raise StoreError(str(error)) from error
        if not isinstance(runtime_manifest, dict):
            raise StoreError("RuntimeArtifact manifest must be an object")
        try:
            require_valid(
                "umbraflow-runtime-artifact-v1.schema.json",
                runtime_manifest,
                "RuntimeArtifact manifest",
            )
        except ValueError as error:
            raise StoreError(str(error)) from error
        if runtime_manifest["page_model"]["sha256"] != runtime_model_hash:
            raise StoreError("RuntimeArtifact model does not match the replayed RuntimeModel")
        release_manifest_bytes = self._validate_release_manifest(
            release_manifest,
            candidate_id=candidate_id,
            candidate_revision=candidate_revision,
            expected_predecessor=expected_predecessor,
            runtime_artifact_root_hash=runtime_artifact_root_hash,
        )
        publication_id = _sha256(release_manifest_bytes)
        with self._transaction() as connection:
            self.verify_publication_capability(publication_capability)
            self.verify_replay_policy(replay_policy)
            existing = connection.execute(
                "SELECT * FROM publications WHERE publication_id = ?", (publication_id,)
            ).fetchone()
            if existing is not None:
                head = connection.execute(
                    "SELECT publication_id FROM published_head WHERE singleton = 1"
                ).fetchone()
                if head is not None and head["publication_id"] == publication_id:
                    return self._publication_document(connection, existing)
                raise Conflict("publication identity exists but is not the current head")
            head = connection.execute(
                "SELECT publication_id, generation FROM published_head WHERE singleton = 1"
            ).fetchone()
            if head is None or head["publication_id"] != expected_predecessor:
                raise Conflict("published head predecessor compare-and-swap failed")
            generation = head["generation"] + 1
            if release_manifest["generation"] != generation:
                raise Conflict("release manifest generation is stale")
            candidate_row = connection.execute(
                """SELECT revisions.document FROM candidate_heads AS heads
                   JOIN candidate_revisions AS revisions
                     ON revisions.candidate_id = heads.candidate_id
                    AND revisions.revision = heads.revision
                   WHERE heads.candidate_id = ? AND heads.revision = ?""",
                (candidate_id, candidate_revision),
            ).fetchone()
            if candidate_row is None:
                raise Conflict("candidate head compare-and-swap failed")
            candidate = _object(candidate_row["document"])
            if candidate["open_issues"]:
                raise Conflict("candidate has unresolved issues")
            try:
                _, compiled_hash = compile_runtime_toml(build_runtime_model(candidate))
            except SchemaIssue as error:
                raise StoreError(str(error)) from error
            if compiled_hash != runtime_model_hash:
                raise Conflict("candidate RuntimeModel changed or was compiled differently")
            manifest_assets = {
                (entry["path"], entry["sha256"], entry["size"])
                for entry in runtime_manifest["assets"]
            }
            candidate_assets: set[tuple[str, str, int]] = set()
            for asset in candidate["runtime_assets"]:
                blob = connection.execute(
                    "SELECT size, kind, asset_type FROM blobs WHERE sha256 = ?",
                    (asset["sha256"],),
                ).fetchone()
                if (
                    blob is None
                    or blob["kind"] != "runtime_asset"
                    or blob["asset_type"] != asset["asset_type"]
                ):
                    raise Conflict("candidate deployable asset identity changed")
                candidate_assets.add((asset["path"], asset["sha256"], blob["size"]))
            if manifest_assets != candidate_assets:
                raise Conflict("RuntimeArtifact assets do not exactly close over the candidate")
            review = connection.execute(
                """SELECT decision_id, outcome FROM review_decisions
                   WHERE candidate_id = ? AND candidate_revision = ?""",
                (candidate_id, candidate_revision),
            ).fetchone()
            if review is None or review["outcome"] != "accepted":
                raise Conflict("candidate revision has no accepted human review")
            replay_rows = self._replay_rows(
                connection,
                candidate_id,
                candidate_revision,
                runtime_model_hash,
                replay_policy,
                replay_result_ids,
                require_unconsumed=True,
            )
            project_row = self._project_operation_row(
                connection,
                candidate_id,
                candidate_revision,
                replay_policy,
                project_operation_result_id,
                require_unconsumed=True,
            )
            gate = self._gate_document(replay_policy, replay_rows, project_row)
            gate_hash = _sha256(jcs_bytes(gate))
            if release_manifest["replay_gate_hash"] != gate_hash:
                raise Conflict("release manifest references another replay gate")
            created_at = _now()
            connection.execute(
                """INSERT INTO publications(
                       publication_id, generation, predecessor_publication_id,
                       candidate_id, candidate_revision, review_decision_id,
                       runtime_model_hash, runtime_artifact_root_hash, replay_gate_hash,
                       runtime_manifest, release_manifest, export_name,
                       publisher_principal, publication_capability_hash, created_at
                   ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)""",
                (
                    publication_id,
                    generation,
                    expected_predecessor,
                    candidate_id,
                    candidate_revision,
                    review["decision_id"],
                    runtime_model_hash,
                    runtime_artifact_root_hash,
                    gate_hash,
                    runtime_manifest_bytes,
                    release_manifest_bytes,
                    publication_id,
                    publication_capability.principal,
                    publication_capability.sha256,
                    created_at,
                ),
            )
            for replay in replay_rows:
                connection.execute(
                    """INSERT INTO replay_attestations(
                           attestation_id, replay_result_id, publication_id,
                           candidate_id, candidate_revision, runtime_model_hash,
                           kind, corpus_hash, replay_policy_hash, passed, report_hash,
                           runner_principal, capability_hash, created_at
                       ) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?)""",
                    (
                        replay["result_id"],
                        replay["result_id"],
                        publication_id,
                        candidate_id,
                        candidate_revision,
                        runtime_model_hash,
                        replay["kind"],
                        replay["corpus_hash"],
                        replay["replay_policy_hash"],
                        replay["report_hash"],
                        replay["runner_principal"],
                        replay["capability_hash"],
                        created_at,
                    ),
                )
            connection.execute(
                """INSERT INTO project_operation_attestations(
                       attestation_id, replay_result_id, publication_id, replay_bundle_id,
                       candidate_id, candidate_revision, replay_policy_hash, passed,
                       report_hash, runner_principal, capability_hash, created_at
                   ) VALUES(?, ?, ?, ?, ?, ?, ?, 1, ?, ?, ?, ?)""",
                (
                    project_row["result_id"],
                    project_row["result_id"],
                    publication_id,
                    project_row["replay_bundle_id"],
                    candidate_id,
                    candidate_revision,
                    project_row["replay_policy_hash"],
                    project_row["report_hash"],
                    project_row["runner_principal"],
                    project_row["capability_hash"],
                    created_at,
                ),
            )
            changed = connection.execute(
                """UPDATE published_head SET publication_id = ?, generation = ?
                   WHERE singleton = 1 AND generation = ?
                     AND ((publication_id IS NULL AND ? IS NULL) OR publication_id = ?)""",
                (
                    publication_id,
                    generation,
                    head["generation"],
                    expected_predecessor,
                    expected_predecessor,
                ),
            ).rowcount
            if changed != 1:
                raise Conflict("published head compare-and-swap failed")
            stored = connection.execute(
                "SELECT * FROM publications WHERE publication_id = ?", (publication_id,)
            ).fetchone()
            assert stored is not None
            return self._publication_document(connection, stored)

    @staticmethod
    def _publication_document(connection: sqlite3.Connection, row: sqlite3.Row) -> dict[str, Any]:
        attestations = connection.execute(
            """SELECT attestation_id, replay_result_id, publication_id, candidate_id,
                      candidate_revision, runtime_model_hash, kind, corpus_hash,
                      replay_policy_hash, passed, report_hash, runner_principal,
                      capability_hash, created_at
               FROM replay_attestations WHERE publication_id = ?
               ORDER BY CASE kind WHEN 'frame' THEN 0 ELSE 1 END""",
            (row["publication_id"],),
        ).fetchall()
        return {
            "publication_id": row["publication_id"],
            "generation": row["generation"],
            "predecessor_publication_id": row["predecessor_publication_id"],
            "candidate_id": row["candidate_id"],
            "candidate_revision": row["candidate_revision"],
            "review_decision_id": row["review_decision_id"],
            "runtime_model_hash": row["runtime_model_hash"],
            "runtime_artifact_root_hash": row["runtime_artifact_root_hash"],
            "replay_gate_hash": row["replay_gate_hash"],
            "runtime_manifest": _object(row["runtime_manifest"]),
            "release_manifest": _object(row["release_manifest"]),
            "export_name": row["export_name"],
            "publisher_principal": row["publisher_principal"],
            "publication_capability_hash": row["publication_capability_hash"],
            "created_at": row["created_at"],
            "replay_attestations": [
                {**dict(item), "passed": bool(item["passed"])} for item in attestations
            ],
        }

    def published_release(self) -> dict[str, Any] | None:
        with contextlib.closing(self._connect()) as connection:
            row = connection.execute(
                """SELECT publications.* FROM published_head
                   JOIN publications ON publications.publication_id = published_head.publication_id
                   WHERE published_head.singleton = 1"""
            ).fetchone()
            return None if row is None else self._publication_document(connection, row)

    def committed_publications(self) -> list[dict[str, Any]]:
        with contextlib.closing(self._connect()) as connection:
            rows = connection.execute("SELECT * FROM publications ORDER BY generation").fetchall()
            return [self._publication_document(connection, row) for row in rows]

    def referenced_artifact_roots(self) -> set[str]:
        with contextlib.closing(self._connect()) as connection:
            return {
                row[0]
                for row in connection.execute("SELECT runtime_artifact_root_hash FROM publications")
            }

    def garbage_collect_unreferenced_blobs(self) -> None:
        """Reliable GC for failed/crashed Agent uploads, serialized with all mutations."""

        with self.exclusive():
            with self._transaction() as connection:
                unreferenced = connection.execute(
                    """SELECT sha256, size, kind, asset_type FROM blobs AS blobs
                       WHERE NOT EXISTS(
                           SELECT 1 FROM candidate_blob_refs WHERE sha256 = blobs.sha256
                       ) AND NOT EXISTS(
                           SELECT 1 FROM agent_checkpoint_blob_refs WHERE sha256 = blobs.sha256
                       ) AND NOT EXISTS(
                           SELECT 1 FROM replay_bundle_blob_refs WHERE sha256 = blobs.sha256
                       )"""
                ).fetchall()
                connection.executemany(
                    "DELETE FROM blobs WHERE sha256 = ?", [(row["sha256"],) for row in unreferenced]
                )
            for row in unreferenced:
                reference = BlobRef(row["sha256"], row["size"], row["kind"], row["asset_type"])
                path = self._blob_path(reference.sha256, reference.kind, reference.asset_type)
                if os.path.lexists(path):
                    self._verify_blob_file(reference)
                    path.unlink()

            expected: set[Path] = set()
            with contextlib.closing(self._connect()) as connection:
                rows = connection.execute("SELECT sha256, kind, asset_type FROM blobs").fetchall()
            for row in rows:
                expected.add(self._blob_path(row["sha256"], row["kind"], row["asset_type"]))
            for base in (self.evidence_blobs, *(
                self.runtime_blobs / value for value in sorted(_ASSET_TYPES)
            )):
                for prefix in existing_entries(base):
                    if not prefix.is_dir():
                        raise StoreError(f"unexpected blob namespace entry: {prefix}")
                    if not re.fullmatch(r"[0-9a-f]{2}", prefix.name):
                        raise StoreError(f"unexpected blob prefix directory: {prefix}")
                    for path in existing_entries(prefix):
                        if path not in expected:
                            require_plain_file(path)
                            if _SHA256.fullmatch(path.name) is None:
                                raise StoreError(f"unexpected blob file name: {path}")
                            content = read_plain_file(path, maximum=_MAX_RUNTIME_ASSET_BYTES)
                            if _sha256(content) != path.name:
                                raise StoreError(f"unregistered blob has a false content address: {path}")
                            path.unlink()
                    if not any(existing_entries(prefix)):
                        prefix.rmdir()
