"""Trusted CLI for bootstrap, human review, replay attestation, and publication."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

from .jcs import CanonicalJsonError, jcs_bytes, load_exact_jcs
from .publication import Publisher
from .safe_paths import UnsafePath, paths_overlap, read_plain_file, require_plain_file
from .store import (
    APPLICATION_ID,
    SCHEMA_ROOT_HASH,
    SCHEMA_VERSION,
    AnnotationStore,
    AuthoringCapabilityRoot,
    PublicationCapability,
    ReplayPolicy,
    StoreError,
    authority_paths_hash,
    open_human_review_capability,
    open_publication_capability,
    open_replay_policy,
    open_replay_runner_capability,
)


def _outside_workspace(workspace: Path, protected_file: Path, name: str) -> None:
    if paths_overlap(workspace.absolute(), protected_file.absolute()):
        raise StoreError(f"{name} must be outside the authoring workspace")


def _initialize(arguments: argparse.Namespace) -> dict[str, Any]:
    workspace = arguments.store.absolute()
    for path, name in (
        (arguments.human_capability, "human review capability"),
        (arguments.replay_capability, "replay runner capability"),
        (arguments.publication_capability, "publication capability"),
        (arguments.replay_policy, "replay policy"),
    ):
        _outside_workspace(workspace, path, name)
    human = open_human_review_capability(arguments.human_capability)
    try:
        replay = open_replay_runner_capability(arguments.replay_capability)
        try:
            publication = open_publication_capability(arguments.publication_capability)
            try:
                policy = open_replay_policy(arguments.replay_policy)
                try:
                    workspace_identity = {
                        "authority_paths_hash": authority_paths_hash(
                            arguments.human_capability,
                            arguments.replay_capability,
                            arguments.publication_capability,
                            arguments.replay_policy,
                        ),
                        "human_review_capability_hash": human.sha256,
                        "publication_capability_hash": publication.sha256,
                        "replay_policy_hash": policy.exact_hash,
                        "replay_runner_capability_hash": replay.sha256,
                    }
                    root = AuthoringCapabilityRoot(
                        workspace_id=hashlib.sha256(jcs_bytes(workspace_identity)).hexdigest(),
                        human_review_capability_hash=human.sha256,
                        replay_runner_capability_hash=replay.sha256,
                        publication_capability_hash=publication.sha256,
                        replay_policy_hash=policy.exact_hash,
                    )
                    store = AnnotationStore.initialize(
                        workspace,
                        root,
                        authority_paths_digest=workspace_identity["authority_paths_hash"],
                    )
                    return {
                        "application_id": APPLICATION_ID,
                        "database": str(store.database),
                        "schema_root_hash": SCHEMA_ROOT_HASH,
                        "user_version": SCHEMA_VERSION,
                        "workspace_id": root.workspace_id,
                    }
                finally:
                    policy.close()
            finally:
                publication.close()
        finally:
            replay.close()
    finally:
        human.close()


def _review(arguments: argparse.Namespace) -> dict[str, Any]:
    store = AnnotationStore(arguments.store)
    with open_human_review_capability(arguments.capability) as capability:
        return store.review_candidate(
            capability,
            arguments.candidate_id,
            arguments.candidate_revision,
            arguments.outcome,
            arguments.comment,
        )


def _load_exact_object(path: Path, name: str) -> dict[str, Any]:
    try:
        content = read_plain_file(path.absolute(), maximum=16 * 1024 * 1024)
        value = load_exact_jcs(content)
    except (UnsafePath, CanonicalJsonError) as error:
        raise StoreError(f"{name} must be one exact JCS plain file") from error
    if not isinstance(value, dict):
        raise StoreError(f"{name} must contain one object")
    return value


def _record_replay(arguments: argparse.Namespace) -> dict[str, Any]:
    store = AnnotationStore(arguments.store)
    _outside_workspace(store.root, arguments.result, "trusted replay result input")
    result = _load_exact_object(arguments.result, "trusted replay result")
    with open_replay_runner_capability(arguments.replay_capability) as capability:
        with open_replay_policy(arguments.replay_policy) as policy:
            return store.record_trusted_replay_result(capability, policy, result)


def _record_bundle(arguments: argparse.Namespace) -> dict[str, Any]:
    store = AnnotationStore(arguments.store)
    _outside_workspace(store.root, arguments.bundle, "replay bundle input")
    bundle = _load_exact_object(arguments.bundle, "replay bundle")
    with open_replay_runner_capability(arguments.replay_capability) as capability:
        return store.record_replay_bundle(capability, bundle)


def _record_project_replay(arguments: argparse.Namespace) -> dict[str, Any]:
    store = AnnotationStore(arguments.store)
    _outside_workspace(store.root, arguments.result, "project/operation replay result input")
    result = _load_exact_object(arguments.result, "project/operation replay result")
    with open_replay_runner_capability(arguments.replay_capability) as capability:
        with open_replay_policy(arguments.replay_policy) as policy:
            return store.record_project_operation_replay_result(capability, policy, result)


def _publisher(
    arguments: argparse.Namespace,
) -> tuple[Publisher, PublicationCapability, ReplayPolicy]:
    store = AnnotationStore(arguments.store)
    for path in (arguments.human_capability_path, arguments.replay_capability_path):
        try:
            require_plain_file(path.absolute())
        except UnsafePath as error:
            raise StoreError("protected authority path is not one plain file") from error
    publication = open_publication_capability(arguments.publication_capability)
    try:
        policy = open_replay_policy(arguments.replay_policy)
        try:
            publisher = Publisher(
                store,
                arguments.handoff,
                publication,
                policy,
                human_review_capability_path=arguments.human_capability_path,
                replay_runner_capability_path=arguments.replay_capability_path,
            )
        except BaseException:
            policy.close()
            raise
    except BaseException:
        publication.close()
        raise
    return publisher, publication, policy


def _publish(arguments: argparse.Namespace) -> dict[str, Any]:
    publisher, publication, policy = _publisher(arguments)
    try:
        return publisher.publish(
            arguments.candidate_id,
            arguments.candidate_revision,
            arguments.expected_predecessor,
            (arguments.frame_result_id, arguments.transition_result_id),
            arguments.project_operation_result_id,
        )
    finally:
        publication.close()
        policy.close()


def _recover(arguments: argparse.Namespace) -> dict[str, Any]:
    publisher, publication, policy = _publisher(arguments)
    try:
        publisher.recover()
        return {"recovered": True}
    finally:
        publication.close()
        policy.close()


def _add_publisher_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--store", required=True, type=Path)
    parser.add_argument("--handoff", required=True, type=Path)
    parser.add_argument("--publication-capability", required=True, type=Path)
    parser.add_argument("--replay-policy", required=True, type=Path)
    parser.add_argument("--human-capability-path", required=True, type=Path)
    parser.add_argument("--replay-capability-path", required=True, type=Path)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)

    initialize = commands.add_parser("init", help="create one exact v2 workspace")
    initialize.add_argument("--store", required=True, type=Path)
    initialize.add_argument("--human-capability", required=True, type=Path)
    initialize.add_argument("--replay-capability", required=True, type=Path)
    initialize.add_argument("--publication-capability", required=True, type=Path)
    initialize.add_argument("--replay-policy", required=True, type=Path)
    initialize.set_defaults(callback=_initialize)

    review = commands.add_parser("review", help="record one immutable human decision")
    review.add_argument("--store", required=True, type=Path)
    review.add_argument("--capability", required=True, type=Path)
    review.add_argument("--candidate-id", required=True)
    review.add_argument("--candidate-revision", required=True, type=int)
    review.add_argument("--outcome", required=True, choices=("accepted", "rejected"))
    review.add_argument("--comment", default="")
    review.set_defaults(callback=_review)

    replay = commands.add_parser(
        "record-replay", help="trusted runner fixes one replay result identity before publication"
    )
    replay.add_argument("--store", required=True, type=Path)
    replay.add_argument("--replay-capability", required=True, type=Path)
    replay.add_argument("--replay-policy", required=True, type=Path)
    replay.add_argument("--result", required=True, type=Path)
    replay.set_defaults(callback=_record_replay)

    bundle = commands.add_parser(
        "record-bundle", help="assemble one offline Replay Bundle closure under the runner capability"
    )
    bundle.add_argument("--store", required=True, type=Path)
    bundle.add_argument("--replay-capability", required=True, type=Path)
    bundle.add_argument("--bundle", required=True, type=Path)
    bundle.set_defaults(callback=_record_bundle)

    project_replay = commands.add_parser(
        "record-project-replay",
        help="trusted runner fixes one project/operation replay result over one bundle",
    )
    project_replay.add_argument("--store", required=True, type=Path)
    project_replay.add_argument("--replay-capability", required=True, type=Path)
    project_replay.add_argument("--replay-policy", required=True, type=Path)
    project_replay.add_argument("--result", required=True, type=Path)
    project_replay.set_defaults(callback=_record_project_replay)

    publish = commands.add_parser("publish", help="consume replay intents and commit a release")
    _add_publisher_arguments(publish)
    publish.add_argument("--candidate-id", required=True)
    publish.add_argument("--candidate-revision", required=True, type=int)
    publish.add_argument("--expected-predecessor")
    publish.add_argument("--frame-result-id", required=True)
    publish.add_argument("--transition-result-id", required=True)
    publish.add_argument("--project-operation-result-id", required=True)
    publish.set_defaults(callback=_publish)

    recover = commands.add_parser("recover", help="recover orphans under the publication lock")
    _add_publisher_arguments(recover)
    recover.set_defaults(callback=_recover)

    arguments = parser.parse_args(argv)
    result = arguments.callback(arguments)
    print(json.dumps(result, ensure_ascii=False, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
