"""Cross-repository parity gate for the frozen Umbraflow interface lock.

The consumer lock is intentionally supplied by path.  It is not copied into
this repository and it is never used as a production schema search path.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any, Iterable

from jsonschema import Draft202012Validator
from referencing import Registry, Resource


SCHEMA_ID_PREFIX = "https://umbraflow.dev/schema/"


class ParityFailure(AssertionError):
    pass


def load(path: Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def schema_registry(schema_paths: Iterable[Path]) -> Registry:
    registry = Registry()
    for path in schema_paths:
        document = load(path)
        registry = registry.with_resource(
            document["$id"], Resource.from_contents(document)
        )
    return registry


def validator(schema_path: Path, registry: Registry) -> Draft202012Validator:
    return Draft202012Validator(load(schema_path), registry=registry)


def field_set(document: dict[str, Any]) -> set[str]:
    return set(document)


def error_pointer(errors: Iterable[Any]) -> str:
    first = next(iter(errors), None)
    if first is None:
        return "/document"
    suffix = "/".join(
        str(part).replace("~", "~0").replace("/", "~1")
        for part in first.absolute_path
    )
    return "/document" + (f"/{suffix}" if suffix else "")


def require_source_tokens(path: Path, vector: str, tokens: Iterable[str]) -> None:
    source = path.read_text(encoding="utf-8")
    missing = [token for token in tokens if token not in source]
    if missing:
        raise ParityFailure(
            f"{vector}: current producer surface is missing fields {missing} in {path}"
        )


def validate_fact_vectors(
    vectors: Path, schema_root: Path, registry: Registry
) -> None:
    fact_validator = validator(schema_root / "umbraflow-fact-v1.schema.json", registry)
    cases = load(vectors / "facts.json")["cases"]
    for case in cases:
        errors = list(fact_validator.iter_errors(case["document"]))
        actual = "valid" if not errors else "invalid"
        if actual != case["expected"]:
            raise ParityFailure(
                "facts.json: case "
                f"{case['name']} disagrees at {error_pointer(errors)}; "
                f"expected {case['expected']}, "
                f"current producer schema reports {actual}"
            )


def collection_semantically_valid(document: dict[str, Any]) -> bool:
    metadata = document.get("metadata", {})
    if metadata.get("status") not in {"Known", "Stale"}:
        return True
    value = metadata.get("value")
    if not isinstance(value, dict) or not isinstance(value.get("item_count"), int):
        return True
    item_count = value["item_count"]
    item_length = len(document.get("items", []))
    completeness = document.get("completeness")
    if completeness == "Complete":
        return item_count == item_length
    return item_count >= item_length


def validate_collection_vectors(
    vectors: Path, schema_root: Path, registry: Registry
) -> None:
    collection_validator = validator(
        schema_root / "umbraflow-collection-fact-v1.schema.json", registry
    )
    cases = load(vectors / "collection-facts.json")["cases"]
    for case in cases:
        document = case["document"]
        valid = collection_validator.is_valid(document) and collection_semantically_valid(
            document
        )
        actual = "valid" if valid else "invalid"
        if actual != case["expected"]:
            raise ParityFailure(
                "collection-facts.json: case "
                f"{case['name']} disagrees at /document; expected {case['expected']}, "
                f"current producer contract reports {actual}"
            )


def validate_observed_instance_surface(
    vectors: Path, frozen_schemas: Path, source_root: Path, registry: Registry
) -> None:
    document = load(vectors / "observed-instances.json")
    authority_validator = validator(
        frozen_schemas / "umbraflow-observed-instance-authority-input-v1.schema.json",
        registry,
    )
    for case in document["mint_cases"]:
        if not authority_validator.is_valid(case["input"]):
            raise ParityFailure(
                f"observed-instances.json: {case['name']} disagrees at /input"
            )
    for case in document["invalid_authority_cases"]:
        if authority_validator.is_valid(case["input"]):
            raise ParityFailure(
                f"observed-instances.json: {case['name']} unexpectedly admits /input"
            )
    require_source_tokens(
        source_root / "modules/operator/source/operator/ledger.cpp",
        "observed-instances.json",
        (
            "identity_schema_id",
            "kind",
            "plugin_id",
            "project_instance_key",
            "project_registration_hash",
            "schema",
            "semantic_identity_basis",
            "world_scope",
            "generation",
            "scope_id",
        ),
    )


def validate_project_plugin_surface(
    vectors: Path, frozen_schemas: Path, source_root: Path, registry: Registry
) -> None:
    document = load(vectors / "project-plugin.json")
    proposal_validator = validator(
        frozen_schemas / "umbraflow-project-observation-proposal-v1.schema.json",
        registry,
    )
    observation_validator = validator(
        frozen_schemas / "umbraflow-project-observation-v1.schema.json", registry
    )
    if not proposal_validator.is_valid(document["proposal"]):
        raise ParityFailure("project-plugin.json: producer disagrees at /proposal")
    if not observation_validator.is_valid(document["expected_observation"]):
        raise ParityFailure(
            "project-plugin.json: producer disagrees at /expected_observation"
        )
    require_source_tokens(
        source_root / "modules/operator/source/operator/project-observation.hpp",
        "project-plugin.json",
        (
            "canonicalOpaquePayload",
            "projectToolPreconditions",
            "observedInstanceProposals",
            "observedInstances",
            "semanticIdentityBasis",
            "opaqueProjectPayload",
        ),
    )


def validate_workflow_vector(
    vectors: Path, schema_root: Path, registry: Registry
) -> None:
    vector = load(vectors / "single-step-tool.json")["valid"]
    workflow_schema = schema_root / "umbraflow-declarative-workflow-tool-v1.schema.json"
    workflow_validator = validator(workflow_schema, registry)
    if workflow_validator.is_valid(vector):
        return

    expected_top = set(load(workflow_schema)["required"])
    actual_top = field_set(vector)
    missing = sorted(expected_top - actual_top)
    stale = sorted(actual_top - expected_top)
    expected_bounds = set(load(workflow_schema)["properties"]["bounds"]["required"])
    actual_bounds = field_set(vector.get("bounds", {}))
    missing_bounds = sorted(expected_bounds - actual_bounds)
    stale_bounds = sorted(actual_bounds - expected_bounds)
    raise ParityFailure(
        "single-step-tool.json: /valid/schema is "
        f"{vector.get('schema')!r}, current producer requires "
        "'umbraflow-declarative-workflow-tool/v1'; "
        f"/valid missing fields {missing}, stale fields {stale}; "
        f"/valid/bounds missing fields {missing_bounds}, stale fields {stale_bounds}"
    )


def run(lock_root: Path, source_root: Path, schema_root: Path) -> list[str]:
    vectors = lock_root / "vectors"
    frozen_schemas = lock_root / "schemas"
    registry = schema_registry([*frozen_schemas.glob("*.json"), *schema_root.glob("*.json")])
    checks = (
        (
            "facts.json",
            lambda: validate_fact_vectors(vectors, schema_root, registry),
        ),
        (
            "collection-facts.json",
            lambda: validate_collection_vectors(vectors, schema_root, registry),
        ),
        (
            "observed-instances.json",
            lambda: validate_observed_instance_surface(
                vectors, frozen_schemas, source_root, registry
            ),
        ),
        (
            "project-plugin.json",
            lambda: validate_project_plugin_surface(
                vectors, frozen_schemas, source_root, registry
            ),
        ),
        (
            "single-step-tool.json",
            lambda: validate_workflow_vector(vectors, schema_root, registry),
        ),
    )
    failures: list[str] = []
    for name, check in checks:
        try:
            check()
        except (ParityFailure, KeyError, TypeError, ValueError) as error:
            failures.append(str(error))
            print(f"FAIL {name}: {error}")
        else:
            print(f"PASS {name}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--lock-root", type=Path, required=True)
    parser.add_argument(
        "--source-root", type=Path, default=Path(__file__).resolve().parents[2]
    )
    parser.add_argument("--schema-root", type=Path)
    args = parser.parse_args()
    schema_root = args.schema_root or args.source_root / "schema"
    failures = run(args.lock_root.resolve(), args.source_root.resolve(), schema_root.resolve())
    if failures:
        print(f"PARITY: FAIL ({len(failures)}/5 vectors diverge)")
        return 1
    print("PARITY: PASS (5/5 vectors agree)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
