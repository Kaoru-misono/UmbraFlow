"""Trusted RuntimeArtifact publication and deployment-neutral handoff."""

from __future__ import annotations

import hashlib
import os
import re
import tempfile
from pathlib import Path, PurePosixPath
from typing import Any, Mapping, Sequence

from .candidate_model import build_runtime_model, validate_candidate_model
from .contracts import require_valid
from .jcs import CanonicalJsonError, jcs_bytes, load_exact_jcs
from .model_file import SchemaIssue, compile_runtime_toml
from .safe_paths import (
    UnsafePath,
    confined_relative,
    existing_entries,
    make_plain_directories,
    paths_overlap,
    read_plain_file,
    remove_plain_tree,
    require_plain_directory,
    require_plain_file,
    walk_plain_files,
    write_new_file,
)
from .store import (
    ANNOTATION_WORKSPACE_FORMAT,
    SCHEMA_VERSION,
    AnnotationStore,
    Conflict,
    PublicationCapability,
    ReplayPolicy,
    StoreError,
    authority_paths_hash,
)


_SCHEMA_ROOT = Path(__file__).resolve().parents[2] / "schema"
# The two generations a RuntimeArtifact manifest declares: the v1 in
# umbraflow-runtime-artifact-v1.schema.json and the v2 in
# umbraflow-runtime-v2.schema.json. The Host reads the same two numbers as
# k_runtimeArtifactFormat and k_runtimeModelFormat. They are generations rather
# than digests of those two files so that editing either file's prose does not
# refuse every artifact already published against it.
RUNTIME_ARTIFACT_FORMAT = 1
RUNTIME_MODEL_FORMAT = 2
_MANIFEST_NAME = "runtime-artifact.manifest.json"
_MODEL_NAME = "runtime-model.toml"
_RELEASE_MANIFEST_NAME = "release.manifest.json"
_RUNTIME_DIRECTORY = "runtime-artifact"
_SHA256 = re.compile(r"^[0-9a-f]{64}$")
_MAX_MODEL_BYTES = 4_194_304
_MAX_ASSET_BYTES = 268_435_456
_MAX_ASSETS = 4096


def _sha256(content: bytes) -> str:
    return hashlib.sha256(content).hexdigest()


def _read(path: Path, *, maximum: int | None = None) -> bytes:
    try:
        return read_plain_file(path, maximum=maximum)
    except UnsafePath as error:
        raise StoreError(f"required plain file is unsafe: {path}") from error


def _directory(path: Path) -> None:
    try:
        require_plain_directory(path)
    except UnsafePath as error:
        raise StoreError(f"required directory is unsafe: {path}") from error


def _write(path: Path, content: bytes) -> None:
    try:
        make_plain_directories(path.parent)
        write_new_file(path, content)
    except (OSError, UnsafePath) as error:
        raise StoreError(f"cannot create one plain immutable file: {path}") from error


def _asset_path(raw: Any) -> PurePosixPath:
    try:
        return confined_relative(raw, prefix="assets")
    except UnsafePath as error:
        raise StoreError(str(error)) from error


def _runtime_assets(candidate: Mapping[str, Any]) -> dict[str, tuple[str, str]]:
    assets: dict[str, tuple[str, str]] = {}
    for reference in candidate["runtime_assets"]:
        path = _asset_path(reference["path"]).as_posix()
        digest = reference["sha256"]
        asset_type = reference["asset_type"]
        if not isinstance(digest, str) or _SHA256.fullmatch(digest) is None:
            raise StoreError(f"runtime asset {path!r} has an invalid private SHA-256")
        if path in assets:
            raise Conflict(f"runtime asset path {path!r} is duplicated")
        assets[path] = (digest, asset_type)
    referenced = {
        _asset_path(locator["asset_path"]).as_posix()
        for locator in candidate["runtime_model"]["locators"]
        if locator["kind"] == "template"
    }
    if set(assets) != referenced:
        raise Conflict("candidate assets do not exactly close over RuntimeModel template locators")
    return assets


class Publisher:
    """Compile, consume trusted replay intents, commit, and export one release."""

    def __init__(
        self,
        store: AnnotationStore,
        handoff_root: Path | str,
        publication_capability: PublicationCapability,
        replay_policy: ReplayPolicy,
        *,
        human_review_capability_path: Path | str,
        replay_runner_capability_path: Path | str,
    ) -> None:
        self.store = store
        self.handoff_root = Path(handoff_root).absolute()
        self.publication_capability = publication_capability
        self.replay_policy = replay_policy
        human_path = Path(human_review_capability_path).absolute()
        replay_path = Path(replay_runner_capability_path).absolute()
        pinned_paths = authority_paths_hash(
            human_path,
            replay_path,
            publication_capability.path,
            replay_policy.path,
        )
        if pinned_paths != self.store.pinned_authority_paths_hash():
            raise StoreError("authority/capability/policy path set is not pinned by this workspace")
        self.protected_paths = tuple(
            dict.fromkeys(
                [
                    self.store.root,
                    human_path,
                    replay_path,
                    publication_capability.path,
                    replay_policy.path,
                ]
            )
        )
        self.store.verify_publication_capability(publication_capability)
        self.store.verify_replay_policy(replay_policy)
        self._require_separate_handoff()
        make_plain_directories(self.handoff_root)
        make_plain_directories(self.handoff_root / ".staging")
        with self.store.exclusive():
            self._require_separate_handoff()
            self._validate_handoff_root(allow_missing_committed=True, allow_orphans=True)
            self._recover_locked()
            self._validate_handoff_root(allow_missing_committed=False)

    def _require_separate_handoff(self) -> None:
        for protected in self.protected_paths:
            if paths_overlap(self.handoff_root, protected):
                raise StoreError(
                    f"handoff root overlaps authoring/capability/policy/replay path: {protected}"
                )

    @staticmethod
    def _manifest_from_exact(manifest_bytes: bytes) -> dict[str, Any]:
        try:
            manifest = load_exact_jcs(manifest_bytes)
        except CanonicalJsonError as error:
            raise StoreError(str(error)) from error
        if not isinstance(manifest, dict):
            raise StoreError("RuntimeArtifact manifest must be one exact JCS object")
        try:
            require_valid(
                "umbraflow-runtime-artifact-v1.schema.json",
                manifest,
                "RuntimeArtifact manifest",
            )
        except ValueError as error:
            raise StoreError(str(error)) from error
        assets = manifest["assets"]
        if len(assets) > _MAX_ASSETS:
            raise StoreError("RuntimeArtifact asset count exceeds the Host ceiling")
        paths = [item["path"] for item in assets]
        if paths != sorted(paths, key=lambda value: value.encode("utf-8")):
            raise StoreError("RuntimeArtifact assets are not sorted by UTF-8 path")
        if len(paths) != len(set(paths)):
            raise StoreError("RuntimeArtifact asset paths are not unique")
        if manifest["page_model"]["size"] <= 0 or manifest["page_model"]["size"] > _MAX_MODEL_BYTES:
            raise StoreError("RuntimeArtifact model violates the Host size ceiling")
        if any(item["size"] <= 0 or item["size"] > _MAX_ASSET_BYTES for item in assets):
            raise StoreError("RuntimeArtifact asset violates the Host size ceiling")
        return manifest

    @classmethod
    def _verify_runtime_artifact(
        cls,
        directory: Path,
        manifest_bytes: bytes,
        artifact_root: str,
    ) -> dict[str, Any]:
        manifest = cls._manifest_from_exact(manifest_bytes)
        if _sha256(manifest_bytes) != artifact_root:
            raise Conflict("RuntimeArtifact root does not match exact manifest bytes")
        if manifest["runtime_artifact_format"] != RUNTIME_ARTIFACT_FORMAT:
            raise Conflict("RuntimeArtifact manifest format is not the one this publisher writes")
        if manifest["runtime_model_format"] != RUNTIME_MODEL_FORMAT:
            raise Conflict("RuntimeArtifact RuntimeModel format is not the one this publisher writes")
        expected = {_MANIFEST_NAME, manifest["page_model"]["path"]}
        expected.update(item["path"] for item in manifest["assets"])
        try:
            actual = walk_plain_files(directory)
        except UnsafePath as error:
            raise StoreError(f"RuntimeArtifact has an unsafe hierarchy: {directory}") from error
        if actual != expected:
            raise Conflict(f"RuntimeArtifact {artifact_root} has an unexpected file closure")
        if _read(directory / _MANIFEST_NAME) != manifest_bytes:
            raise Conflict("RuntimeArtifact manifest bytes changed")
        for item in [manifest["page_model"], *manifest["assets"]]:
            path = confined_relative(item["path"])
            content = _read(
                directory.joinpath(*path.parts),
                maximum=_MAX_MODEL_BYTES if item["path"] == _MODEL_NAME else _MAX_ASSET_BYTES,
            )
            if len(content) != item["size"] or _sha256(content) != item["sha256"]:
                raise Conflict(f"RuntimeArtifact file {item['path']!r} is corrupt")
        return manifest

    def _materialize_runtime_artifact(
        self,
        model_content: bytes,
        assets: Mapping[str, tuple[str, str]],
    ) -> tuple[dict[str, Any], bytes, str]:
        if not model_content or len(model_content) > _MAX_MODEL_BYTES:
            raise StoreError("compiled RuntimeModel violates the RuntimeArtifact schema ceiling")
        if len(assets) > _MAX_ASSETS:
            raise StoreError("candidate has too many deployable assets")
        temporary = Path(tempfile.mkdtemp(prefix="artifact-", dir=self.store.staging))
        _directory(temporary)
        model_entry = {
            "path": _MODEL_NAME,
            "sha256": _sha256(model_content),
            "size": len(model_content),
        }
        entries: list[dict[str, Any]] = []
        try:
            _write(temporary / _MODEL_NAME, model_content)
            for relative, (digest, asset_type) in sorted(
                assets.items(), key=lambda item: item[0].encode("utf-8")
            ):
                content = self.store.read_blob(digest, "runtime_asset", asset_type)
                if not content or len(content) > _MAX_ASSET_BYTES:
                    raise StoreError("candidate deployable asset violates the Host size ceiling")
                destination = temporary.joinpath(*_asset_path(relative).parts)
                _write(destination, content)
                entries.append({"path": relative, "sha256": digest, "size": len(content)})
            manifest = {
                "assets": entries,
                "page_model": model_entry,
                "runtime_artifact_format": RUNTIME_ARTIFACT_FORMAT,
                "runtime_model_format": RUNTIME_MODEL_FORMAT,
            }
            try:
                require_valid(
                    "umbraflow-runtime-artifact-v1.schema.json",
                    manifest,
                    "RuntimeArtifact manifest",
                )
            except ValueError as error:
                raise StoreError(str(error)) from error
            manifest_bytes = jcs_bytes(manifest)
            artifact_root = _sha256(manifest_bytes)
            _write(temporary / _MANIFEST_NAME, manifest_bytes)
            self._verify_runtime_artifact(temporary, manifest_bytes, artifact_root)
        except BaseException:
            if os.path.lexists(temporary):
                remove_plain_tree(temporary)
            raise

        destination = self.store.objects / artifact_root
        if os.path.lexists(destination):
            self._verify_runtime_artifact(destination, manifest_bytes, artifact_root)
            remove_plain_tree(temporary)
        else:
            try:
                os.replace(temporary, destination)
            except OSError:
                if not os.path.lexists(destination):
                    raise
                self._verify_runtime_artifact(destination, manifest_bytes, artifact_root)
                remove_plain_tree(temporary)
            self._verify_runtime_artifact(destination, manifest_bytes, artifact_root)
        return manifest, manifest_bytes, artifact_root

    def _verify_export(self, directory: Path, publication: Mapping[str, Any]) -> None:
        release_bytes = jcs_bytes(publication["release_manifest"])
        if _read(directory / _RELEASE_MANIFEST_NAME) != release_bytes:
            raise Conflict("handoff release manifest bytes changed")
        runtime = directory / _RUNTIME_DIRECTORY
        runtime_bytes = jcs_bytes(publication["runtime_manifest"])
        self._verify_runtime_artifact(
            runtime,
            runtime_bytes,
            publication["runtime_artifact_root_hash"],
        )
        expected = {_RELEASE_MANIFEST_NAME}
        try:
            expected.update(
                f"{_RUNTIME_DIRECTORY}/{path}" for path in walk_plain_files(runtime)
            )
            actual = walk_plain_files(directory)
        except UnsafePath as error:
            raise StoreError("handoff export contains an unsafe hierarchy") from error
        if actual != expected:
            raise Conflict("handoff contains files outside the committed release closure")

    def _export(self, publication: Mapping[str, Any]) -> Path:
        destination = self.handoff_root / publication["export_name"]
        if os.path.lexists(destination):
            self._verify_export(destination, publication)
            return destination
        temporary = Path(
            tempfile.mkdtemp(prefix="export-", dir=self.handoff_root / ".staging")
        )
        _directory(temporary)
        try:
            _write(
                temporary / _RELEASE_MANIFEST_NAME,
                jcs_bytes(publication["release_manifest"]),
            )
            source = self.store.objects / publication["runtime_artifact_root_hash"]
            runtime_destination = temporary / _RUNTIME_DIRECTORY
            make_plain_directories(runtime_destination)
            try:
                source_files = sorted(walk_plain_files(source), key=lambda value: value.encode("utf-8"))
            except UnsafePath as error:
                raise StoreError("committed RuntimeArtifact contains an unsafe hierarchy") from error
            for relative in source_files:
                path = confined_relative(relative)
                _write(runtime_destination.joinpath(*path.parts), _read(source.joinpath(*path.parts)))
            self._verify_export(temporary, publication)
            try:
                os.replace(temporary, destination)
            except OSError:
                if not os.path.lexists(destination):
                    raise
                self._verify_export(destination, publication)
                remove_plain_tree(temporary)
            self._verify_export(destination, publication)
            return destination
        except BaseException:
            if os.path.lexists(temporary):
                remove_plain_tree(temporary)
            raise

    def _validate_handoff_root(
        self,
        *,
        allow_missing_committed: bool,
        allow_orphans: bool = False,
    ) -> None:
        """Validate the entire handoff namespace, not only the selected export."""

        self._require_separate_handoff()
        _directory(self.handoff_root)
        _directory(self.handoff_root / ".staging")
        publications = self.store.committed_publications()
        by_name = {row["export_name"]: row for row in publications}
        present: set[str] = set()
        try:
            entries = list(existing_entries(self.handoff_root))
        except UnsafePath as error:
            raise StoreError("handoff root contains an unsafe entry") from error
        for entry in entries:
            if entry.name == ".staging":
                _directory(entry)
                continue
            if _SHA256.fullmatch(entry.name) is None:
                raise StoreError(f"handoff root contains an uncommitted entry: {entry.name}")
            if entry.name not in by_name:
                if not allow_orphans:
                    raise StoreError(f"handoff root contains an uncommitted entry: {entry.name}")
                _directory(entry)
                continue
            _directory(entry)
            self._verify_export(entry, by_name[entry.name])
            present.add(entry.name)
        if not allow_missing_committed and present != set(by_name):
            raise StoreError("handoff root is missing a committed release export")

    def _clear_staging(self, directory: Path) -> None:
        try:
            entries = list(existing_entries(directory))
        except UnsafePath as error:
            raise StoreError("staging contains an unsafe path") from error
        for entry in entries:
            metadata = entry.lstat()
            if metadata.st_nlink == 1 and entry.is_file():
                require_plain_file(entry)
                entry.unlink()
            else:
                remove_plain_tree(entry)

    def _recover_locked(self) -> None:
        self._validate_handoff_root(allow_missing_committed=True, allow_orphans=True)
        self._clear_staging(self.store.staging)
        self._clear_staging(self.handoff_root / ".staging")

        publications = self.store.committed_publications()
        committed_artifacts = {row["runtime_artifact_root_hash"] for row in publications}
        try:
            object_entries = list(existing_entries(self.store.objects))
        except UnsafePath as error:
            raise StoreError("RuntimeArtifact object root contains an unsafe entry") from error
        for entry in object_entries:
            if _SHA256.fullmatch(entry.name) is None:
                raise StoreError(f"unexpected RuntimeArtifact object name: {entry.name}")
            _directory(entry)
            if entry.name not in committed_artifacts:
                remove_plain_tree(entry)

        committed_exports = {row["export_name"] for row in publications}
        try:
            handoff_entries = list(existing_entries(self.handoff_root))
        except UnsafePath as error:
            raise StoreError("handoff root contains an unsafe entry") from error
        for entry in handoff_entries:
            if entry.name == ".staging":
                continue
            if _SHA256.fullmatch(entry.name) is None:
                raise StoreError(f"unexpected handoff entry: {entry.name}")
            if entry.name not in committed_exports:
                remove_plain_tree(entry)
        for publication in publications:
            self._export(publication)

    def recover(self) -> None:
        """Serialize recovery with publish so active pre-commit staging cannot be removed."""

        with self.store.exclusive():
            self.store.verify_publication_capability(self.publication_capability)
            self.store.verify_replay_policy(self.replay_policy)
            self._recover_locked()
            self._validate_handoff_root(allow_missing_committed=False)

    def publish(
        self,
        candidate_id: str,
        candidate_revision: int,
        expected_predecessor_publication_id: str | None,
        replay_result_ids: Sequence[str],
        project_operation_result_id: Any,
    ) -> dict[str, Any]:
        with self.store.exclusive():
            self.store.verify_publication_capability(self.publication_capability)
            self.store.verify_replay_policy(self.replay_policy)
            self._require_separate_handoff()
            self._validate_handoff_root(allow_missing_committed=False)
            candidate = self.store.get_candidate(candidate_id, candidate_revision)
            current = self.store.get_candidate(candidate_id)
            if current["revision"] != candidate_revision:
                raise Conflict("candidate head compare-and-swap failed")
            diagnostics = validate_candidate_model(candidate)
            if diagnostics:
                first = diagnostics[0]
                raise StoreError(f"candidate is invalid at {first['path']}: {first['message']}")
            if candidate["open_issues"]:
                raise Conflict("candidate has unresolved issues")
            review = self.store.review_for(candidate_id, candidate_revision)
            if review is None or review["outcome"] != "accepted":
                raise Conflict("candidate revision has no accepted human review")

            try:
                model_content, model_hash = compile_runtime_toml(build_runtime_model(candidate))
            except SchemaIssue as error:
                raise StoreError(str(error)) from error
            gate = self.store.build_replay_gate(
                replay_policy=self.replay_policy,
                candidate_id=candidate_id,
                candidate_revision=candidate_revision,
                runtime_model_hash=model_hash,
                replay_result_ids=replay_result_ids,
                project_operation_result_id=project_operation_result_id,
            )
            gate_hash = _sha256(jcs_bytes(gate))
            runtime_manifest, runtime_manifest_bytes, artifact_root = (
                self._materialize_runtime_artifact(model_content, _runtime_assets(candidate))
            )
            head = self.store.publication_head()
            if head["publication_id"] != expected_predecessor_publication_id:
                raise Conflict("published head predecessor compare-and-swap failed")
            release_manifest = {
                "annotation_workspace_format": ANNOTATION_WORKSPACE_FORMAT,
                "candidate_id": candidate_id,
                "candidate_revision": candidate_revision,
                "generation": head["generation"] + 1,
                "predecessor_publication_id": expected_predecessor_publication_id,
                "replay_gate_hash": gate_hash,
                "runtime_artifact_root_hash": artifact_root,
                "workspace_sqlite_revision": SCHEMA_VERSION,
            }
            publication = self.store.commit_publication(
                publication_capability=self.publication_capability,
                replay_policy=self.replay_policy,
                candidate_id=candidate_id,
                candidate_revision=candidate_revision,
                expected_predecessor=expected_predecessor_publication_id,
                runtime_model_hash=model_hash,
                runtime_artifact_root_hash=artifact_root,
                runtime_manifest_bytes=runtime_manifest_bytes,
                release_manifest=release_manifest,
                replay_result_ids=replay_result_ids,
                project_operation_result_id=project_operation_result_id,
            )
            self._export(publication)
            self._validate_handoff_root(allow_missing_committed=False)
            return publication
