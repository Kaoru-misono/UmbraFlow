from __future__ import annotations

import base64
import contextlib
import copy
import hashlib
import http.client
import inspect
import json
import multiprocessing
import os
import sqlite3
import stat
import tempfile
import threading
import time
import unittest
from pathlib import Path

from jsonschema import Draft202012Validator
from referencing import Registry, Resource

from tools.annotate import model_file
from tools.annotate.contracts import validate as validate_contract
from tools.annotate.jcs import CanonicalJsonError, jcs_bytes, load_exact_jcs
from tools.annotate.model_file import compile_runtime_toml
from tools.annotate.publication import Publisher
from tools.annotate.safe_paths import is_reparse, make_plain_directories, walk_plain_files
from tools.annotate.serve import (
    CAPABILITIES,
    AgentBackend,
    BackendError,
    load_agent_bearer,
    make_server,
)
from tools.annotate.store import (
    APPLICATION_ID,
    SCHEMA_ROOT_HASH,
    SCHEMA_VERSION,
    AnnotationStore,
    AuthoringCapabilityRoot,
    BlobUpload,
    Conflict,
    HumanReviewCapability,
    PublicationCapability,
    ReplayPolicy,
    ReplayRunnerCapability,
    StoreError,
    authority_paths_hash,
    open_human_review_capability,
    open_publication_capability,
    open_replay_policy,
    open_replay_runner_capability,
)


EVIDENCE_BYTES = b"private annotation screenshot"
PNG_BYTES = base64.b64decode(
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII="
)
FRAME_CORPUS = hashlib.sha256(b"fixed frame replay corpus").hexdigest()
TRANSITION_CORPUS = hashlib.sha256(b"fixed transition replay corpus").hexdigest()


def digest(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def write_jcs(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(jcs_bytes(value))


def capability_document(purpose: str, principal: str) -> dict[str, object]:
    return {
        "capability_version": 1,
        "nonce": digest(f"{purpose}:{principal}:nonce".encode()),
        "principal": principal,
        "purpose": purpose,
    }


def runtime_model(*, threshold: float = 0.9, with_asset: bool = True) -> dict:
    if with_asset:
        locators = [
            {
                "id": "confirm-template",
                "kind": "template",
                "asset_path": "assets/templates/confirm.png",
                "threshold": threshold,
            }
        ]
        readers: list[dict] = []
        detector = {
            "all": [{"kind": "locator_present", "locator": "confirm-template"}],
            "any": [],
            "none": [],
        }
        placement = {
            "kind": "fixed",
            "rect": [10, 20, 100, 40],
            "action_point": [60, 40],
        }
        actions = [{"id": "click", "kind": "click", "proof_locator": "confirm-template"}]
    else:
        locators = []
        readers = [
            {
                "id": "caption-reader",
                "kind": "text",
                "confidence_floor": 0.8,
                "normalization": "trim",
            }
        ]
        detector = {
            "all": [{"kind": "text_equals", "reader": "caption-reader", "value": "Ready"}],
            "any": [],
            "none": [],
        }
        placement = {"kind": "fixed", "rect": [10, 20, 100, 40]}
        actions = []
    return {
        "schema_version": 2,
        "base_resolution": [1920, 1080],
        "base_dpi": [96, 96],
        "ui_targets": [{"id": "confirm-button", "kind": "control"}],
        "locators": locators,
        "readers": readers,
        "bindings": [
            {
                "id": "camp-confirm",
                "surface": "camp-scene",
                "ui_target": "confirm-button",
                "variant": "default",
                "placement": placement,
                "detector": detector,
                "actions": actions,
            }
        ],
        "surfaces": [
            {
                "id": "camp-scene",
                "kind": "scene",
                "covers": [],
                "identity": {"all": ["camp-confirm"], "any": [], "none": []},
            }
        ],
        "transitions": [],
    }


def candidate(
    evidence_hash: str,
    *,
    candidate_id: str = "candidate-1",
    revision: int = 1,
    threshold: float = 0.9,
    with_asset: bool = True,
    open_issues: list[str] | None = None,
) -> dict:
    assets = (
        [
            {
                "asset_type": "template_png",
                "path": "assets/templates/confirm.png",
                "sha256": digest(PNG_BYTES),
            }
        ]
        if with_asset
        else []
    )
    return {
        "id": candidate_id,
        "revision": revision,
        "proposed_by": "annotation-agent",
        "evidence_blob_hashes": [evidence_hash],
        "runtime_assets": assets,
        "runtime_model": runtime_model(threshold=threshold, with_asset=with_asset),
        "open_issues": [] if open_issues is None else open_issues,
    }


class Workspace:
    def __init__(self, base: Path) -> None:
        self.base = base
        self.root = base / "authoring-private"
        self.handoff = base / "deployment-handoff"
        self.authorities = base / "authority-files"
        self.human_path = self.authorities / "human.jcs"
        self.replay_path = self.authorities / "replay.jcs"
        self.publication_path = self.authorities / "publication.jcs"
        self.policy_path = self.authorities / "replay-policy.jcs"
        write_jcs(self.human_path, capability_document("human-review", "human:alice"))
        write_jcs(self.replay_path, capability_document("replay-runner", "runner:ci"))
        write_jcs(
            self.publication_path,
            capability_document("publication", "publisher:release"),
        )
        write_jcs(
            self.policy_path,
            {
                "frame_corpus_hash": FRAME_CORPUS,
                "policy_version": 1,
                "transition_corpus_hash": TRANSITION_CORPUS,
            },
        )
        self.human = open_human_review_capability(self.human_path)
        self.replay = open_replay_runner_capability(self.replay_path)
        self.publication = open_publication_capability(self.publication_path)
        self.policy = open_replay_policy(self.policy_path)
        pinned_authority_paths = authority_paths_hash(
            self.human_path,
            self.replay_path,
            self.publication_path,
            self.policy_path,
        )
        identity = {
            "authority_paths_hash": pinned_authority_paths,
            "human_review_capability_hash": self.human.sha256,
            "publication_capability_hash": self.publication.sha256,
            "replay_policy_hash": self.policy.exact_hash,
            "replay_runner_capability_hash": self.replay.sha256,
        }
        root = AuthoringCapabilityRoot(
            workspace_id=digest(jcs_bytes(identity)),
            human_review_capability_hash=self.human.sha256,
            replay_runner_capability_hash=self.replay.sha256,
            publication_capability_hash=self.publication.sha256,
            replay_policy_hash=self.policy.exact_hash,
        )
        self.store = AnnotationStore.initialize(
            self.root,
            root,
            authority_paths_digest=pinned_authority_paths,
        )
        self.add_candidate()

    def close(self) -> None:
        self.human.close()
        self.replay.close()
        self.publication.close()
        self.policy.close()

    def add_candidate(
        self,
        *,
        candidate_id: str = "candidate-1",
        revision: int = 1,
        expected_revision: int | None = None,
        evidence: bytes | None = None,
        threshold: float = 0.9,
        with_asset: bool = True,
    ) -> dict:
        evidence = (
            EVIDENCE_BYTES + candidate_id.encode() + str(revision).encode()
            if evidence is None
            else evidence
        )
        document = candidate(
            digest(evidence),
            candidate_id=candidate_id,
            revision=revision,
            threshold=threshold,
            with_asset=with_asset,
        )
        uploads = [BlobUpload("evidence", evidence)]
        if with_asset:
            uploads.append(BlobUpload("runtime_asset", PNG_BYTES, "template_png"))
        return self.store.commit_candidate_with_uploads(document, expected_revision, uploads)

    def accept(self, candidate_id: str = "candidate-1", revision: int = 1) -> dict:
        return self.store.review_candidate(
            self.human, candidate_id, revision, "accepted", "checked"
        )

    def attest(self, candidate_id: str = "candidate-1", revision: int = 1) -> tuple[str, str]:
        document = self.store.get_candidate(candidate_id, revision)
        _, model_hash = compile_runtime_toml(document["runtime_model"])
        result_ids: list[str] = []
        for kind, corpus in (
            ("frame", FRAME_CORPUS),
            ("transition", TRANSITION_CORPUS),
        ):
            result = self.store.record_trusted_replay_result(
                self.replay,
                self.policy,
                {
                    "candidate_id": candidate_id,
                    "candidate_revision": revision,
                    "runtime_model_hash": model_hash,
                    "kind": kind,
                    "corpus_hash": corpus,
                    "passed": True,
                    "report": {"failures": [], "kind": kind},
                },
            )
            result_ids.append(result["result_id"])
        return result_ids[0], result_ids[1]

    def publisher(self, handoff: Path | None = None) -> Publisher:
        return Publisher(
            self.store,
            self.handoff if handoff is None else handoff,
            self.publication,
            self.policy,
            human_review_capability_path=self.human_path,
            replay_runner_capability_path=self.replay_path,
        )


class WorkspaceTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.workspace = Workspace(Path(self.temporary.name))

    def tearDown(self) -> None:
        self.workspace.close()
        self.temporary.cleanup()


def _hold_workspace_lock(root: str, ready: Any, release: Any) -> None:
    store = AnnotationStore(Path(root))
    with store.exclusive():
        active = store.staging / "active-precommit"
        active.mkdir()
        (active / "marker").write_bytes(b"precommit")
        ready.set()
        release.wait(10)


def _recover_in_process(
    root: str,
    handoff: str,
    publication_path: str,
    policy_path: str,
    human_path: str,
    replay_path: str,
    result: Any,
) -> None:
    publication = open_publication_capability(publication_path)
    policy = open_replay_policy(policy_path)
    try:
        store = AnnotationStore(root)
        Publisher(
            store,
            handoff,
            publication,
            policy,
            human_review_capability_path=human_path,
            replay_runner_capability_path=replay_path,
        ).recover()
        result.put("ok")
    except BaseException as error:
        result.put(f"{type(error).__name__}: {error}")
    finally:
        publication.close()
        policy.close()


class SchemaAndJcsTests(unittest.TestCase):
    def test_official_draft_202012_runtime_validation_matches_direct_validator(self) -> None:
        schema_path = Path("schema/umbraflow-runtime-v2.schema.json")
        schema = json.loads(schema_path.read_text(encoding="utf-8"))
        direct = Draft202012Validator(schema)
        cases = [
            runtime_model(),
            {**runtime_model(), "extra": True},
            {**runtime_model(), "base_resolution": [0, 1080]},
            {**runtime_model(), "locators": [{**runtime_model()["locators"][0], "threshold": 2}]},
        ]
        for value in cases:
            with self.subTest(value=value):
                official_errors = validate_contract("umbraflow-runtime-v2.schema.json", value)
                self.assertEqual(bool(official_errors), not direct.is_valid(value))
        self.assertFalse(hasattr(model_file, "RuntimeSchema"))

    def test_runtime_artifact_schema_accepts_zero_assets_and_enforces_all_ceilings(self) -> None:
        sha = "a" * 64
        valid = {
            "manifest_schema_hash": sha,
            "runtime_model_schema_hash": sha,
            "page_model": {"path": "page-model.toml", "size": 1, "sha256": sha},
            "assets": [],
        }
        self.assertEqual(validate_contract("umbraflow-runtime-artifact-v1.schema.json", valid), [])
        for mutation in (
            {**valid, "page_model": {**valid["page_model"], "size": 0}},
            {**valid, "page_model": {**valid["page_model"], "size": 4_194_305}},
            {**valid, "extra": True},
            {
                **valid,
                "assets": [
                    {"path": f"assets/{index}.png", "size": 1, "sha256": sha}
                    for index in range(4097)
                ],
            },
            {
                **valid,
                "assets": [{"path": "assets/a.png", "size": 0, "sha256": sha}],
            },
        ):
            with self.subTest(mutation=mutation):
                self.assertTrue(validate_contract("umbraflow-runtime-artifact-v1.schema.json", mutation))

    def test_annotation_schema_uses_phase_and_requires_frame_then_transition(self) -> None:
        schema_path = Path("schema/umbraflow-annotation-workspace-v2.schema.json")
        raw = schema_path.read_text(encoding="utf-8")
        schema = json.loads(raw)
        Draft202012Validator.check_schema(schema)
        self.assertNotIn('"stage"', raw)
        checkpoint = {
            "candidate": None,
            "evidence_blob_hashes": [],
            "input_artifact_roots": [],
            "job_id": "job",
            "resume_state": {},
            "revision": 1,
            "phase": "collecting",
        }
        self.assertTrue(
            Draft202012Validator(schema, registry=_schema_registry()).is_valid(checkpoint)
        )

        sha = "a" * 64
        common = {
            "attestation_id": sha,
            "replay_result_id": sha,
            "publication_id": "b" * 64,
            "candidate_id": "candidate",
            "candidate_revision": 1,
            "runtime_model_hash": sha,
            "corpus_hash": sha,
            "replay_policy_hash": sha,
            "passed": True,
            "report_hash": sha,
            "runner_principal": "runner",
            "capability_hash": sha,
            "created_at": "2026-08-09T00:00:00Z",
        }
        publication_schema = {
            "$schema": "https://json-schema.org/draft/2020-12/schema",
            "$defs": schema["$defs"],
            "$ref": "#/$defs/Publication/properties/replay_attestations",
        }
        validator = Draft202012Validator(publication_schema, registry=_schema_registry())
        correct = [{**common, "kind": "frame"}, {**common, "kind": "transition"}]
        self.assertTrue(validator.is_valid(correct))
        self.assertFalse(validator.is_valid([{**common, "kind": "frame"}] * 2))
        self.assertFalse(validator.is_valid(list(reversed(correct))))

    def test_jcs_exact_vectors_and_noncanonical_input(self) -> None:
        self.assertEqual(jcs_bytes({"b": 1, "a": 2}), b'{"a":2,"b":1}')
        self.assertEqual(
            jcs_bytes({"\ue000": 2, "😀": 1}).decode("utf-8"),
            '{"😀":1,"\ue000":2}',
        )
        self.assertEqual(
            jcs_bytes({"control": "\b\t\n\f\r\"\\"}),
            b'{"control":"\\b\\t\\n\\f\\r\\\"\\\\"}',
        )
        with self.assertRaises(CanonicalJsonError):
            load_exact_jcs(b'{"a": 1}')
        with self.assertRaises(CanonicalJsonError):
            jcs_bytes({"float": 1.5})
        for value in ({"a": 1}, {"a": [True, None, "x"]}, {"a": {"b": -2}}):
            expected = json.dumps(
                value, ensure_ascii=False, sort_keys=True, separators=(",", ":")
            ).encode()
            self.assertEqual(jcs_bytes(value), expected)


def _schema_registry() -> Registry:
    registry = Registry()
    for name in (
        "umbraflow-annotation-workspace-v2.schema.json",
        "umbraflow-runtime-v2.schema.json",
        "umbraflow-runtime-artifact-v1.schema.json",
    ):
        schema = json.loads((Path("schema") / name).read_text(encoding="utf-8"))
        registry = registry.with_resource(schema["$id"], Resource.from_contents(schema))
        registry = registry.with_resource(name, Resource.from_contents(schema))
    return registry


class ExactDatabaseTests(WorkspaceTestCase):
    def test_database_has_exact_identity_tables_and_triggers(self) -> None:
        store = self.workspace.store
        with contextlib.closing(sqlite3.connect(store.database)) as connection:
            self.assertEqual(connection.execute("PRAGMA application_id").fetchone()[0], APPLICATION_ID)
            self.assertEqual(connection.execute("PRAGMA user_version").fetchone()[0], SCHEMA_VERSION)
            self.assertEqual(connection.execute("PRAGMA journal_mode").fetchone()[0].lower(), "wal")
            tables = {
                row[0]
                for row in connection.execute(
                    "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%'"
                )
            }
            self.assertEqual(
                tables,
                {
                    "agent_checkpoint_blob_refs",
                    "agent_checkpoints",
                    "authoring_capability_root",
                    "blobs",
                    "candidate_blob_refs",
                    "candidate_heads",
                    "candidate_revisions",
                    "publications",
                    "published_head",
                    "replay_attestations",
                    "replay_result_intents",
                    "review_decisions",
                },
            )
            triggers = {
                row[0]
                for row in connection.execute("SELECT name FROM sqlite_schema WHERE type='trigger'")
            }
            self.assertIn("candidate_revisions_immutable_update", triggers)
            self.assertIn("replay_result_intents_immutable_delete", triggers)
            self.assertIn("publications_immutable_update", triggers)
            self.assertIn("replay_attestations_immutable_delete", triggers)
        self.assertRegex(SCHEMA_ROOT_HASH, r"^[0-9a-f]{64}$")

    def test_schema_and_application_drift_are_rejected_without_migration(self) -> None:
        self.workspace.close()
        with contextlib.closing(sqlite3.connect(self.workspace.store.database)) as connection:
            connection.execute("CREATE TABLE compatibility_alias(value TEXT) STRICT")
            connection.commit()
        with self.assertRaisesRegex(StoreError, "exactly match"):
            AnnotationStore(self.workspace.root)
        self.workspace.human = open_human_review_capability(self.workspace.human_path)
        self.workspace.replay = open_replay_runner_capability(self.workspace.replay_path)
        self.workspace.publication = open_publication_capability(self.workspace.publication_path)
        self.workspace.policy = open_replay_policy(self.workspace.policy_path)

    def test_immutable_rows_reject_raw_update_and_delete(self) -> None:
        self.workspace.accept()
        replay_ids = self.workspace.attest()
        publication = self.workspace.publisher().publish("candidate-1", 1, None, replay_ids)
        attacks = (
            "UPDATE candidate_revisions SET document = '{}' WHERE candidate_id = 'candidate-1'",
            f"DELETE FROM replay_result_intents WHERE result_id = '{replay_ids[0]}'",
            f"UPDATE publications SET generation = 9 WHERE publication_id = '{publication['publication_id']}'",
            "DELETE FROM review_decisions WHERE candidate_id = 'candidate-1'",
            f"UPDATE replay_attestations SET kind = 'transition' WHERE replay_result_id = '{replay_ids[0]}'",
            "UPDATE blobs SET kind = 'evidence' WHERE kind = 'runtime_asset'",
        )
        for statement in attacks:
            with self.subTest(statement=statement):
                with contextlib.closing(sqlite3.connect(self.workspace.store.database)) as connection:
                    with self.assertRaises(sqlite3.IntegrityError):
                        connection.execute(statement)


class CapabilitySecurityTests(WorkspaceTestCase):
    def test_authority_and_policy_cannot_be_directly_constructed(self) -> None:
        with self.assertRaises(StoreError):
            HumanReviewCapability(Path("x"), -1, b"", {}, _token=object())
        with self.assertRaises(StoreError):
            ReplayRunnerCapability(Path("x"), -1, b"", {}, _token=object())
        with self.assertRaises(StoreError):
            PublicationCapability(Path("x"), -1, b"", {}, _token=object())
        with self.assertRaises(StoreError):
            ReplayPolicy(Path("x"), -1, b"", {}, _token=object())
        self.assertFalse(hasattr(__import__("tools.annotate.store", fromlist=["x"]), "AuthorityCapability"))

    def test_three_purposes_are_type_isolated(self) -> None:
        with self.assertRaisesRegex(StoreError, "wrong trusted capability type"):
            self.workspace.store.review_candidate(
                self.workspace.publication, "candidate-1", 1, "accepted", "forged"
            )
        document = self.workspace.store.get_candidate("candidate-1", 1)
        _, model_hash = compile_runtime_toml(document["runtime_model"])
        with self.assertRaisesRegex(StoreError, "wrong trusted capability type"):
            self.workspace.store.record_trusted_replay_result(
                self.workspace.human,
                self.workspace.policy,
                {
                    "candidate_id": "candidate-1",
                    "candidate_revision": 1,
                    "runtime_model_hash": model_hash,
                    "kind": "frame",
                    "corpus_hash": FRAME_CORPUS,
                    "passed": True,
                    "report": {},
                },
            )
        wrong = self.workspace.base / "wrong-purpose.jcs"
        write_jcs(wrong, capability_document("publication", "attacker"))
        with self.assertRaisesRegex(StoreError, "purpose"):
            open_human_review_capability(wrong)

    def test_verify_rereads_exact_capability_and_policy_descriptor_bytes(self) -> None:
        replacement = jcs_bytes(capability_document("human-review", "human:mallory"))
        try:
            descriptor = os.open(self.workspace.human_path, os.O_WRONLY | os.O_TRUNC)
        except PermissionError:
            self.workspace.human.verify_open()  # Windows handle sharing itself prevents replacement.
        else:
            try:
                os.write(descriptor, replacement)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            with self.assertRaisesRegex(StoreError, "exact bytes|identity"):
                self.workspace.human.verify_open()

        replacement_policy = jcs_bytes(
            {
                "frame_corpus_hash": "0" * 64,
                "policy_version": 1,
                "transition_corpus_hash": TRANSITION_CORPUS,
            }
        )
        try:
            descriptor = os.open(self.workspace.policy_path, os.O_WRONLY | os.O_TRUNC)
        except PermissionError:
            self.workspace.policy.verify_open()
        else:
            try:
                os.write(descriptor, replacement_policy)
                os.fsync(descriptor)
            finally:
                os.close(descriptor)
            with self.assertRaisesRegex(StoreError, "exact bytes|identity"):
                self.workspace.policy.verify_open()


class ReplayProvenanceTests(WorkspaceTestCase):
    def _result(self, **changes: object) -> dict:
        document = self.workspace.store.get_candidate("candidate-1", 1)
        _, model_hash = compile_runtime_toml(document["runtime_model"])
        value = {
            "candidate_id": "candidate-1",
            "candidate_revision": 1,
            "runtime_model_hash": model_hash,
            "kind": "frame",
            "corpus_hash": FRAME_CORPUS,
            "passed": True,
            "report": {"frames": 10},
        }
        value.update(changes)
        return value

    def test_runner_must_prebind_every_replay_identity_field(self) -> None:
        attacks = (
            {"runtime_model_hash": "0" * 64},
            {"candidate_id": "other"},
            {"candidate_revision": 2},
            {"corpus_hash": "0" * 64},
            {"passed": False},
            {"kind": "other"},
            {"extra": True},
        )
        for changes in attacks:
            with self.subTest(changes=changes):
                with self.assertRaises(StoreError):
                    self.workspace.store.record_trusted_replay_result(
                        self.workspace.replay,
                        self.workspace.policy,
                        self._result(**changes),
                    )

    def test_old_model_result_cannot_be_reused_for_new_revision(self) -> None:
        old_ids = self.workspace.attest()
        self.workspace.add_candidate(revision=2, expected_revision=1, threshold=0.8)
        self.workspace.accept(revision=2)
        document = self.workspace.store.get_candidate("candidate-1", 2)
        _, new_hash = compile_runtime_toml(document["runtime_model"])
        transition = self.workspace.store.record_trusted_replay_result(
            self.workspace.replay,
            self.workspace.policy,
            {
                "candidate_id": "candidate-1",
                "candidate_revision": 2,
                "runtime_model_hash": new_hash,
                "kind": "transition",
                "corpus_hash": TRANSITION_CORPUS,
                "passed": True,
                "report": {},
            },
        )
        with self.assertRaisesRegex(Conflict, "identity"):
            self.workspace.publisher().publish(
                "candidate-1", 2, None, (old_ids[0], transition["result_id"])
            )

    def test_publication_has_no_replay_authority_or_raw_result_input(self) -> None:
        constructor = inspect.signature(Publisher)
        self.assertNotIn("replay_capability", constructor.parameters)
        commit = inspect.signature(self.workspace.store.commit_publication)
        self.assertNotIn("replay_capability", commit.parameters)
        self.assertNotIn("replay_results", commit.parameters)
        self.assertIn("replay_result_ids", commit.parameters)
        with self.assertRaisesRegex(StoreError, "cross-process lock"):
            self.workspace.store.commit_publication(
                publication_capability=self.workspace.publication,
                replay_policy=self.workspace.policy,
                candidate_id="candidate-1",
                candidate_revision=1,
                expected_predecessor=None,
                runtime_model_hash="0" * 64,
                runtime_artifact_root_hash="0" * 64,
                runtime_manifest_bytes=b"{}",
                release_manifest={},
                replay_result_ids=("0" * 64, "1" * 64),
            )
        self.workspace.accept()
        with self.assertRaises(StoreError):
            self.workspace.publisher().publish("candidate-1", 1, None, ({"kind": "frame"}, {}))

    def test_replay_intents_are_immutable_and_consumed_once(self) -> None:
        self.workspace.accept()
        ids = self.workspace.attest()
        publication = self.workspace.publisher().publish("candidate-1", 1, None, ids)
        self.assertEqual(
            [row["replay_result_id"] for row in publication["replay_attestations"]],
            list(ids),
        )
        with self.assertRaises(Conflict):
            self.workspace.store.build_replay_gate(
                replay_policy=self.workspace.policy,
                candidate_id="candidate-1",
                candidate_revision=1,
                runtime_model_hash=publication["runtime_model_hash"],
                replay_result_ids=ids,
            )

    def test_agent_surface_cannot_review_replay_or_publish(self) -> None:
        self.assertEqual(
            set(CAPABILITIES),
            {
                "get_agent_checkpoint",
                "get_candidate",
                "list_candidates",
                "propose_candidate",
                "save_agent_checkpoint",
                "validate_candidate",
            },
        )
        backend = AgentBackend(self.workspace.store)
        for forbidden in ("review_candidate", "record_trusted_replay_result", "publish"):
            self.assertFalse(hasattr(backend, forbidden))


class BlobAndAgentTests(WorkspaceTestCase):
    def test_evidence_and_runtime_assets_have_non_reclassifiable_namespaces(self) -> None:
        runtime_digest = digest(PNG_BYTES)
        with self.assertRaisesRegex(Conflict, "another immutable namespace"):
            self.workspace.store.put_evidence_blob(PNG_BYTES)
        runtime_path = (
            self.workspace.store.runtime_blobs
            / "template_png"
            / runtime_digest[:2]
            / runtime_digest
        )
        evidence_path = self.workspace.store.evidence_blobs / runtime_digest[:2] / runtime_digest
        self.assertTrue(runtime_path.is_file())
        self.assertFalse(evidence_path.exists())
        with self.assertRaisesRegex(StoreError, "PNG"):
            self.workspace.store.put_runtime_asset(b"raw screenshot bytes", "template_png")

    def test_failed_agent_upload_leaves_no_database_row_or_file(self) -> None:
        backend = AgentBackend(self.workspace.store)
        failed_bytes = b"new evidence that must be rolled back"
        failed = candidate(digest(failed_bytes), revision=2)
        body = {
            "candidate": failed,
            "expected_revision": 99,
            "blobs": [
                {
                    "kind": "evidence",
                    "content_base64": base64.b64encode(failed_bytes).decode(),
                }
            ],
        }
        with self.assertRaises(BackendError):
            backend.propose_candidate(body)
        failed_hash = digest(failed_bytes)
        with contextlib.closing(sqlite3.connect(self.workspace.store.database)) as connection:
            self.assertIsNone(
                connection.execute("SELECT 1 FROM blobs WHERE sha256 = ?", (failed_hash,)).fetchone()
            )
        self.assertFalse(
            (self.workspace.store.evidence_blobs / failed_hash[:2] / failed_hash).exists()
        )

    def test_startup_gc_removes_crash_orphan_with_verified_content_address(self) -> None:
        content = b"crashed upload before database commit"
        orphan_hash = digest(content)
        orphan = self.workspace.store.evidence_blobs / orphan_hash[:2] / orphan_hash
        orphan.parent.mkdir()
        orphan.write_bytes(content)
        AnnotationStore(self.workspace.root)
        self.assertFalse(orphan.exists())

    def test_empty_evidence_follows_workspace_schema_but_empty_runtime_asset_is_rejected(self) -> None:
        self.workspace.add_candidate(candidate_id="empty-evidence", evidence=b"")
        self.assertEqual(self.workspace.store.read_blob(digest(b""), "evidence"), b"")
        with self.assertRaisesRegex(StoreError, "size"):
            self.workspace.store.put_runtime_asset(b"", "template_png")

    def test_checkpoint_uses_phase_and_rejects_old_stage_alias(self) -> None:
        evidence_hash = self.workspace.store.get_candidate("candidate-1", 1)[
            "evidence_blob_hashes"
        ][0]
        checkpoint = {
            "candidate": {"id": "candidate-1", "revision": 1},
            "evidence_blob_hashes": [evidence_hash],
            "input_artifact_roots": [],
            "job_id": "job-17",
            "resume_state": {},
            "revision": 1,
            "phase": "validating",
        }
        self.workspace.store.save_agent_checkpoint(checkpoint, None)
        self.assertEqual(self.workspace.store.get_agent_checkpoint("job-17")["phase"], "validating")
        old = {**checkpoint, "revision": 2}
        old["stage"] = old.pop("phase")
        with self.assertRaisesRegex(StoreError, "fields"):
            self.workspace.store.save_agent_checkpoint(old, 1)


class AgentHttpSecurityTests(WorkspaceTestCase):
    def setUp(self) -> None:
        super().setUp()
        self.token = base64.urlsafe_b64encode(os.urandom(32)).decode().rstrip("=")
        self.server = make_server(AgentBackend(self.workspace.store), self.token, 0)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.port = int(self.server.server_address[1])

    def tearDown(self) -> None:
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(5)
        super().tearDown()

    def request(self, headers: Mapping[str, str]) -> tuple[int, dict]:
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request("GET", "/api/schema", headers=dict(headers))
            response = connection.getresponse()
            return response.status, json.loads(response.read())
        finally:
            connection.close()

    def test_bearer_host_origin_and_fetch_site_are_all_enforced(self) -> None:
        good = {"Authorization": f"Bearer {self.token}"}
        self.assertEqual(self.request({})[0], 401)
        self.assertEqual(self.request({"Authorization": "Bearer wrong"})[0], 401)
        self.assertEqual(self.request({**good, "Host": "attacker.example"})[0], 403)
        self.assertEqual(self.request({**good, "Origin": "https://attacker.example"})[0], 403)
        self.assertEqual(self.request({**good, "Sec-Fetch-Site": "cross-site"})[0], 403)
        status, body = self.request(good)
        self.assertEqual(status, 200)
        self.assertEqual(set(body["capabilities"]), set(CAPABILITIES))

    def test_bearer_file_is_external_canonical_and_at_least_256_bits(self) -> None:
        external = self.workspace.base / "agent-token"
        external.write_text(self.token, encoding="ascii")
        self.assertEqual(load_agent_bearer(external, self.workspace.root), self.token)
        internal = self.workspace.root / "agent-token"
        internal.write_text(self.token, encoding="ascii")
        with self.assertRaisesRegex(StoreError, "outside"):
            load_agent_bearer(internal, self.workspace.root)
        short = self.workspace.base / "short-token"
        short.write_text("A" * 20, encoding="ascii")
        with self.assertRaisesRegex(StoreError, "256 bits"):
            load_agent_bearer(short, self.workspace.root)


class PublicationBoundaryTests(WorkspaceTestCase):
    def test_end_to_end_export_is_exact_runtime_artifact_plus_release_only(self) -> None:
        self.workspace.accept()
        ids = self.workspace.attest()
        publication = self.workspace.publisher().publish("candidate-1", 1, None, ids)
        export = self.workspace.handoff / publication["export_name"]
        files = walk_plain_files(export)
        self.assertEqual(
            files,
            {
                "release.manifest.json",
                "runtime-artifact/page-model.toml",
                "runtime-artifact/runtime-artifact.manifest.json",
                "runtime-artifact/assets/templates/confirm.png",
            },
        )
        self.assertFalse(any("sqlite" in value or "replay" in value or "blob" in value for value in files))
        manifest_bytes = (export / "runtime-artifact/runtime-artifact.manifest.json").read_bytes()
        self.assertEqual(manifest_bytes, jcs_bytes(publication["runtime_manifest"]))
        self.assertEqual(
            validate_contract(
                "umbraflow-runtime-artifact-v1.schema.json", publication["runtime_manifest"]
            ),
            [],
        )
        aw = json.loads(
            Path("schema/umbraflow-annotation-workspace-v2.schema.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertTrue(Draft202012Validator(aw, registry=_schema_registry()).is_valid(publication))

    def test_zero_asset_runtime_artifact_is_supported_end_to_end(self) -> None:
        self.workspace.add_candidate(candidate_id="text-only", with_asset=False)
        self.workspace.accept("text-only", 1)
        ids = self.workspace.attest("text-only", 1)
        publication = self.workspace.publisher().publish("text-only", 1, None, ids)
        self.assertEqual(publication["runtime_manifest"]["assets"], [])

    def test_handoff_overlap_and_global_injection_are_rejected(self) -> None:
        dummy = self.workspace.base / "dummy-human-path"
        dummy.write_bytes(b"not authority")
        with self.assertRaisesRegex(StoreError, "path set is not pinned"):
            Publisher(
                self.workspace.store,
                self.workspace.handoff,
                self.workspace.publication,
                self.workspace.policy,
                human_review_capability_path=dummy,
                replay_runner_capability_path=self.workspace.replay_path,
            )
        with self.assertRaisesRegex(StoreError, "overlaps"):
            self.workspace.publisher(self.workspace.root / "handoff")
        with self.assertRaisesRegex(StoreError, "overlaps"):
            self.workspace.publisher(self.workspace.authorities)

        clean = self.workspace.publisher()
        (self.workspace.handoff / "injected.txt").write_text("attack", encoding="utf-8")
        self.workspace.accept()
        ids = self.workspace.attest()
        with self.assertRaisesRegex(StoreError, "uncommitted entry"):
            clean.publish("candidate-1", 1, None, ids)

    def test_nested_symlink_or_reparse_is_rejected(self) -> None:
        synthetic = type(
            "Metadata",
            (),
            {"st_mode": stat.S_IFDIR, "st_file_attributes": 0x400},
        )()
        self.assertTrue(is_reparse(synthetic))
        make_plain_directories(self.workspace.handoff)
        make_plain_directories(self.workspace.handoff / ".staging")
        target = self.workspace.base / "outside"
        target.mkdir()
        try:
            os.symlink(target, self.workspace.handoff / ".staging" / "escape", target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("this Windows account cannot create a symlink")
        with self.assertRaises(StoreError):
            self.workspace.publisher()

    def test_startup_recovers_only_well_named_orphans_then_proves_exact_closure(self) -> None:
        make_plain_directories(self.workspace.handoff / ".staging")
        orphan_name = "a" * 64
        orphan = self.workspace.handoff / orphan_name
        orphan.mkdir()
        (orphan / "uncommitted").write_bytes(b"orphan")
        object_orphan = self.workspace.store.objects / ("b" * 64)
        object_orphan.mkdir()
        (object_orphan / "uncommitted").write_bytes(b"orphan")
        self.workspace.publisher()
        self.assertFalse(orphan.exists())
        self.assertFalse(object_orphan.exists())
        self.assertEqual({path.name for path in self.workspace.handoff.iterdir()}, {".staging"})

    def test_recover_waits_for_cross_process_publish_lock(self) -> None:
        self.workspace.publisher()
        context = multiprocessing.get_context("spawn")
        ready = context.Event()
        release = context.Event()
        result = context.Queue()
        holder = context.Process(
            target=_hold_workspace_lock,
            args=(str(self.workspace.root), ready, release),
        )
        holder.start()
        self.assertTrue(ready.wait(10))
        active = self.workspace.store.staging / "active-precommit"
        recover = context.Process(
            target=_recover_in_process,
            args=(
                str(self.workspace.root),
                str(self.workspace.handoff),
                str(self.workspace.publication_path),
                str(self.workspace.policy_path),
                str(self.workspace.human_path),
                str(self.workspace.replay_path),
                result,
            ),
        )
        recover.start()
        time.sleep(0.5)
        self.assertTrue(recover.is_alive())
        self.assertTrue(active.exists(), "recover deleted an active pre-commit staging root")
        release.set()
        holder.join(10)
        recover.join(10)
        self.assertEqual(holder.exitcode, 0)
        self.assertEqual(recover.exitcode, 0)
        self.assertEqual(result.get(timeout=2), "ok")
        self.assertFalse(active.exists())

    def test_two_publishers_with_same_predecessor_have_one_winner(self) -> None:
        self.workspace.add_candidate(candidate_id="candidate-2")
        for identifier in ("candidate-1", "candidate-2"):
            self.workspace.accept(identifier, 1)
        replay = {
            identifier: self.workspace.attest(identifier, 1)
            for identifier in ("candidate-1", "candidate-2")
        }
        publishers = [self.workspace.publisher(), self.workspace.publisher()]
        results: list[tuple[str, str]] = []
        guard = threading.Lock()

        def run(offset: int, identifier: str) -> None:
            try:
                publication = publishers[offset].publish(identifier, 1, None, replay[identifier])
                value = ("ok", publication["publication_id"])
            except Conflict as error:
                value = ("conflict", str(error))
            with guard:
                results.append(value)

        threads = [
            threading.Thread(target=run, args=(0, "candidate-1")),
            threading.Thread(target=run, args=(1, "candidate-2")),
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(15)
        self.assertEqual(sorted(value[0] for value in results), ["conflict", "ok"])
        self.assertEqual(self.workspace.store.publication_head()["generation"], 1)
        self.workspace.publisher().recover()


if __name__ == "__main__":
    unittest.main()
