#!/usr/bin/env python3
"""Reject retired or consumer-specific executable surfaces in the repository."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from collections.abc import Iterator
from pathlib import Path
from typing import Any


RETIRED_PATHS = (
    "entry/cli/check.cpp",
    "entry/cli/check.hpp",
    "entry/cli/file-frame-source.cpp",
    "entry/cli/file-frame-source.hpp",
    "entry/cli/platform/ocr-engine-binding-unsupported.cpp",
    "entry/cli/replay.cpp",
    "entry/cli/replay.hpp",
    "entry/cli/run.cpp",
    "entry/cli/run.hpp",
    "entry/cli/run-unsupported.cpp",
    "entry/cli/run-windows.cpp",
    "modules/task/runtime/hits.luau",
    "modules/task/runtime/mint.luau",
    "modules/task/runtime/navigation.luau",
    "modules/task/runtime/oracle.luau",
    "modules/task/runtime/recognition.luau",
    "modules/task/runtime/regress.luau",
    "modules/task/runtime/replay.luau",
    "schema/umbraflow-annotator-api-v1.schema.json",
    "schema/umbraflow-cpp-envelope-v1.schema.json",
    "schema/umbraflow-offline-v1.schema.json",
    "schema/umbraflow-runtime-v1.schema.json",
    "tests/cli/test-check.cpp",
    "tests/cli/test-file-frame-source.cpp",
    "tests/cli/test-replay.cpp",
    "tests/cli/test-run.cpp",
)

REQUIRED_SAFE_PATHS = (
    "entry/cli/explore.cpp",
    "entry/cli/explore.hpp",
    "entry/cli/project-skeleton.cpp",
    "entry/cli/project-skeleton.hpp",
    "entry/cli/targets.cpp",
    "entry/cli/targets.hpp",
    "modules/task/runtime/project.luau",
    "schema/umbraflow-annotation-workspace-v2.schema.json",
    "schema/umbraflow-journal-v1.schema.json",
    "schema/umbraflow-operator-v1.schema.json",
    "schema/umbraflow-policy-v1.schema.json",
    "schema/umbraflow-project-registration-v1.schema.json",
    "schema/umbraflow-runtime-artifact-v1.schema.json",
    "schema/umbraflow-runtime-v2.schema.json",
    "schema/umbraflow-trace-v2.schema.json",
    "tests/cli/test-args.cpp",
    "tests/cli/test-explore-protocol.cpp",
    "tests/cli/test-project-skeleton.cpp",
    "tests/cli/test-targets.cpp",
)

RETIRED_COMMANDS = frozenset({"check", "replay", "run"})
ALLOWED_COMMANDS = frozenset({"explore", "targets"})
FORBIDDEN_BUSINESS_GLOBALS = frozenset(
    {
        "action",
        "click",
        "ctx",
        "double_click",
        "drag",
        "explore",
        "input",
        "key",
        "key_press",
        "keypress",
        "long_press",
        "model",
        "mouse_down",
        "mouse_move",
        "mouse_up",
        "move_pointer",
        "navigation",
        "observe",
        "press",
        "project",
        "receipt",
        "scroll",
        "type_text",
    }
)
FORBIDDEN_SCHEMA_WORDS = frozenset(
    {
        "battle",
        "camp",
        "card",
        "character",
        "chaos",
        "combat",
        "deck",
        "encounter",
        "enemy",
        "fate",
        "game",
        "gameplay",
        "inventory",
        "item",
        "level",
        "mission",
        "player",
        "quest",
        "relic",
        "reward",
        "shop",
        "skill",
        "stage",
        "weapon",
    }
)
IGNORED_SCHEMA_TEXT_KEYS = frozenset(
    {"$comment", "description", "examples", "pattern", "title"}
)
SOURCE_SUFFIXES = frozenset({".c", ".cc", ".cpp", ".h", ".hpp", ".luau"})
VENDORED_DIRECTORY_NAMES = frozenset({"external", "third_party", "vendor"})
REQUIRED_TEST_TARGETS = frozenset(
    {
        "test-cli",
        "test-controller",
        "test-core",
        "test-domain",
        "test-engine",
        "test-script",
        "test-task",
        "test-trace",
    }
)
REQUIRED_CONTRACT_TARGETS = frozenset({"contract-repository-surface"})

SCHEMA_AUTHORITIES = (
    (
        "modules/trace/source/trace/event.hpp",
        "k_traceSchemaHash",
        "schema/umbraflow-trace-v2.schema.json",
    ),
    (
        "modules/task/source/task/page-model-file.hpp",
        "k_runtimeArtifactSchemaHash",
        "schema/umbraflow-runtime-artifact-v1.schema.json",
    ),
    (
        "modules/task/source/task/page-model-file.hpp",
        "k_runtimeModelSchemaHash",
        "schema/umbraflow-runtime-v2.schema.json",
    ),
    (
        "modules/operator/source/operator/runtime-installation.hpp",
        "k_annotationWorkspaceSchemaHash",
        "schema/umbraflow-annotation-workspace-v2.schema.json",
    ),
)

COMMAND_PATTERN = re.compile(r'Command\s*\{\s*"([a-z0-9-]+)"')
STRING_LITERAL_PATTERN = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
STRING_CONSTANT_PATTERN = re.compile(
    r"\b(?:constexpr\s+)?(?:auto|char\s+(?:const\s+)?)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\"([^\"\\]*)\""
)
FRAMEWORK_GLOBALS_DEFINITION_PATTERN = re.compile(
    r"\bauto\s+frameworkProjectGlobals\s*\(\s*\)\s*"
    r"(?:noexcept\s*)?->[^;{]+\{"
)
RETIRED_CLI_SYMBOL_PATTERN = re.compile(
    r"\b(?:CheckArgs|ReplayArgs|RunArgs|dispatchCheck|dispatchReplay|dispatchRun|"
    r"parseCheckArguments|parseReplayArguments|parseRunArguments|checkProduct|"
    r"replayProduct|runProduct)\b"
)
RETIRED_CLI_PATH_PATTERN = re.compile(
    r"\bcli/(?:check|file-frame-source|replay|run|run-unsupported|run-windows|"
    r"platform/ocr-engine-binding-unsupported)\.(?:cpp|hpp)\b",
    re.IGNORECASE,
)
RETIRED_CLI_TEST_NAME_PATTERN = re.compile(
    r"^test-(?:check|file-frame(?:-source)?|replay|run)(?:[-.]|$)",
    re.IGNORECASE,
)
RETIRED_RUNTIME_SYMBOL_PATTERN = re.compile(
    r"\b(?:ContextDetector|ContextResolution|ContextTruth|RuntimeState|UFR)\b|"
    r"\b(?:find_element|mint_hit|resolve_page)\b|"
    r"\bcontext_(?:detector|resolution|truth)\b|\.ufr\b",
    re.IGNORECASE,
)
RETIRED_TYPE_DECLARATION_PATTERN = re.compile(
    r"\b(?:class|struct|type|using)\s+(?:Element|Hit|Page|UFR)\b"
)
CPP_RUNTIME_PARSER_PATTERN = re.compile(
    r"\b(?:RuntimeModelParser|parseRuntimeModel|parseRuntimeModelEnvelope)\b|"
    r"\btoml::(?:parse|parser)\b"
)
LUAU_RUNTIME_PARSER_PATTERN = re.compile(
    r"\bfunction\s+[A-Za-z_][A-Za-z0-9_]*\.parse\s*\(|"
    r"\bfunction\s+[A-Za-z0-9_]*(?:runtime_model|RuntimeModel|toml)"
    r"[A-Za-z0-9_]*\s*\("
)
HOST_VALIDATION_MARKER_PATTERN = re.compile(
    r"HOST_VALIDATION_TEST\(([A-Za-z_][A-Za-z0-9_]*\."
    r"[A-Za-z_][A-Za-z0-9_]*)\)"
)
CPP_COMMENT_PATTERN = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)
CMAKE_COMMENT_PATTERN = re.compile(r"#[^\n]*")
CPP_NON_CODE_PATTERN = re.compile(
    r"//[^\n]*|/\*.*?\*/|R\"[^\s()\\]{0,16}\(.*?\)[^\s()\\]{0,16}\"|"
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.DOTALL,
)
LUAU_NON_CODE_PATTERN = re.compile(
    r"--\[\[.*?\]\]|--[^\n]*|\[\[.*?\]\]|"
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.DOTALL,
)


def first_party_sources(root: Path) -> Iterator[Path]:
    for source_root_name in ("entry", "modules"):
        source_root = root / source_root_name
        if not source_root.is_dir():
            continue
        for path in source_root.rglob("*"):
            if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
                continue
            relative_parts = path.relative_to(root).parts
            if any(part in VENDORED_DIRECTORY_NAMES for part in relative_parts):
                continue
            yield path


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def executable_text(path: Path, text: str) -> str:
    if path.name == "CMakeLists.txt" or path.suffix.lower() == ".cmake":
        return CMAKE_COMMENT_PATTERN.sub("", text)
    pattern = (
        LUAU_NON_CODE_PATTERN
        if path.suffix.lower() == ".luau"
        else CPP_NON_CODE_PATTERN
    )
    return pattern.sub("", text)


def retired_path_errors(root: Path) -> list[str]:
    errors = [
        f"retired path exists: {relative}"
        for relative in RETIRED_PATHS
        if (root / relative).exists()
    ]
    errors.extend(
        f"required safe surface is missing: {relative}"
        for relative in REQUIRED_SAFE_PATHS
        if not (root / relative).is_file()
    )
    cli_tests = root / "tests/cli"
    if cli_tests.is_dir():
        errors.extend(
            f"retired CLI test exists: {path.relative_to(root).as_posix()}"
            for path in sorted(cli_tests.iterdir())
            if path.is_file() and RETIRED_CLI_TEST_NAME_PATTERN.search(path.name)
        )
    return errors


def cli_surface_errors(root: Path) -> list[str]:
    main_path = root / "entry/cli/main.cpp"
    if not main_path.is_file():
        return ["entry/cli/main.cpp is missing; the production command surface is unauditable"]

    main = CPP_COMMENT_PATTERN.sub("", read_text(main_path))
    commands = COMMAND_PATTERN.findall(main)
    errors: list[str] = []
    if len(commands) != len(set(commands)):
        errors.append("entry/cli/main.cpp registers a duplicate CLI command")

    retired = sorted(set(commands) & RETIRED_COMMANDS)
    if retired:
        errors.append(f"retired CLI commands are registered: {', '.join(retired)}")

    unexpected = sorted(set(commands) - ALLOWED_COMMANDS)
    if unexpected:
        errors.append(
            "unreviewed production CLI commands are registered: "
            + ", ".join(unexpected)
        )

    missing = sorted(ALLOWED_COMMANDS - set(commands))
    if missing:
        errors.append(
            "required safe CLI commands are missing: " + ", ".join(missing)
        )

    for path in sorted((root / "entry").rglob("*")):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES | {".txt"}:
            continue
        text = executable_text(path, read_text(path))
        match = RETIRED_CLI_SYMBOL_PATTERN.search(text)
        if match is not None:
            errors.append(
                f"{path.relative_to(root).as_posix()}: retired CLI symbol "
                f"{match.group(0)!r} is executable again"
            )
        path_match = RETIRED_CLI_PATH_PATTERN.search(text)
        if path_match is not None:
            errors.append(
                f"{path.relative_to(root).as_posix()}: retired CLI source "
                f"{path_match.group(0)!r} is referenced again"
            )
    return errors


def extract_braced_body(text: str, body_at: int) -> str | None:
    depth = 0
    for index in range(body_at, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[body_at + 1 : index]
    return None


def business_global_errors(root: Path) -> list[str]:
    definitions: list[tuple[Path, str, str]] = []
    for path in first_party_sources(root):
        if path.suffix.lower() not in {".cpp", ".cc", ".c"}:
            continue
        text = CPP_COMMENT_PATTERN.sub("", read_text(path))
        for match in FRAMEWORK_GLOBALS_DEFINITION_PATTERN.finditer(text):
            body = extract_braced_body(text, match.end() - 1)
            if body is not None:
                definitions.append((path, body, text))

    if len(definitions) != 1:
        return [
            "frameworkProjectGlobals must have exactly one first-party definition; "
            f"found {len(definitions)}"
        ]

    path, body, source = definitions[0]
    published = {
        match.group(1).lower() for match in STRING_LITERAL_PATTERN.finditer(body)
    }
    constants = {
        match.group(1): match.group(2).lower()
        for match in STRING_CONSTANT_PATTERN.finditer(source)
    }
    published.update(
        value
        for name, value in constants.items()
        if re.search(rf"\b{re.escape(name)}\b", body) is not None
    )
    forbidden = sorted(published & FORBIDDEN_BUSINESS_GLOBALS)
    if not forbidden:
        return []
    return [
        f"{path.relative_to(root).as_posix()}: business globals publish privileged "
        f"or direct-action names: {', '.join(forbidden)}"
    ]


def retired_runtime_errors(root: Path) -> list[str]:
    errors: list[str] = []
    for path in first_party_sources(root):
        text = executable_text(path, read_text(path))
        for pattern, label in (
            (RETIRED_RUNTIME_SYMBOL_PATTERN, "retired Runtime v1 symbol"),
            (RETIRED_TYPE_DECLARATION_PATTERN, "retired Page/Element/Hit/UFR type"),
        ):
            match = pattern.search(text)
            if match is not None:
                errors.append(
                    f"{path.relative_to(root).as_posix()}: {label} "
                    f"{match.group(0)!r}"
                )
    return errors


def trusted_parser_errors(root: Path) -> list[str]:
    errors: list[str] = []
    luau_parsers: list[str] = []
    for path in first_party_sources(root):
        original = read_text(path)
        text = executable_text(path, original)
        relative = path.relative_to(root).as_posix()
        if path.suffix.lower() in {".c", ".cc", ".cpp", ".h", ".hpp"}:
            match = CPP_RUNTIME_PARSER_PATTERN.search(text)
            if match is not None:
                errors.append(
                    f"{relative}: C++ registers/interprets RuntimeModel semantics "
                    f"through {match.group(0)!r}"
                )
            continue

        if "RuntimeModel" in original or "page-model.toml" in original:
            luau_parsers.extend(
                relative for _ in LUAU_RUNTIME_PARSER_PATTERN.finditer(text)
            )

    runtime_schema = root / "schema/umbraflow-runtime-v2.schema.json"
    expected_count = 1 if runtime_schema.is_file() else 0
    if len(luau_parsers) != expected_count:
        errors.append(
            "trusted RuntimeModel parser count is "
            f"{len(luau_parsers)}, expected {expected_count}: "
            + (", ".join(luau_parsers) if luau_parsers else "none")
        )
    return errors


def identifier_words(value: str) -> tuple[str, ...]:
    separated = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", value)
    return tuple(word.lower() for word in re.findall(r"[A-Za-z]+", separated))


def schema_tokens(value: Any, parent_key: str = "") -> Iterator[str]:
    if isinstance(value, dict):
        for key, child in value.items():
            if key not in IGNORED_SCHEMA_TEXT_KEYS:
                yield key
                yield from schema_tokens(child, key)
    elif isinstance(value, list):
        for child in value:
            yield from schema_tokens(child, parent_key)
    elif isinstance(value, str) and parent_key not in IGNORED_SCHEMA_TEXT_KEYS:
        yield value


def generic_schema_errors(root: Path) -> list[str]:
    errors: list[str] = []
    schema_root = root / "schema"
    for path in sorted(schema_root.glob("*.json")):
        try:
            document = json.loads(path.read_bytes())
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            errors.append(f"{path.relative_to(root).as_posix()}: invalid UTF-8 JSON: {error}")
            continue

        for token in schema_tokens(document):
            words = identifier_words(token)
            forbidden = set(words) & FORBIDDEN_SCHEMA_WORDS
            if "run" in words and "state" in words:
                forbidden.add("run-state")
            if "account" in words and "state" in words:
                forbidden.add("account-state")
            if forbidden:
                errors.append(
                    f"{path.relative_to(root).as_posix()}: generic schema contains "
                    f"consumer symbol(s) {', '.join(sorted(forbidden))} in {token!r}"
                )
                break
    return errors


def authority_fields(value: Any, path: tuple[str, ...] = ()) -> Iterator[str]:
    if isinstance(value, dict):
        if path:
            normalized = re.sub(r"[^a-z0-9]", "", path[-1].lower())
            if normalized in {
                "deliveryauthority",
                "hostreceiptauthority",
                "receiptauthority",
            }:
                properties = value.get("properties")
                if isinstance(properties, dict):
                    for field in properties:
                        yield f"{path[-1]}.{field}"
        for key, child in value.items():
            yield from authority_fields(child, (*path, key))
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from authority_fields(child, (*path, str(index)))


def receipt_validation_errors(root: Path) -> list[str]:
    operator_schema = root / "schema/umbraflow-operator-v1.schema.json"
    if not operator_schema.is_file():
        return []

    try:
        schema = json.loads(operator_schema.read_bytes())
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"schema/umbraflow-operator-v1.schema.json is invalid: {error}"]

    expected = set(authority_fields(schema))
    if not expected:
        return [
            "operator schema exists but declares no DeliveryAuthority or "
            "ReceiptAuthority fields"
        ]

    markers: dict[str, set[str]] = {}
    tests_root = root / "tests"
    for path in tests_root.rglob("*"):
        if (
            not path.is_file()
            or path == Path(__file__).resolve()
            or path.suffix.lower() not in {".cpp", ".hpp", ".luau"}
            or "external" in path.relative_to(tests_root).parts
        ):
            continue
        for marker in HOST_VALIDATION_MARKER_PATTERN.findall(read_text(path)):
            markers.setdefault(marker, set()).add(
                path.relative_to(tests_root).as_posix()
            )

    cmake = CMAKE_COMMENT_PATTERN.sub(
        "", read_text(tests_root / "CMakeLists.txt")
    )
    errors: list[str] = []
    for field in sorted(expected):
        sources = markers.get(field, set())
        if not sources:
            errors.append(
                f"Receipt authority field {field} has no "
                f"HOST_VALIDATION_TEST({field}) marker"
            )
            continue
        if not any(source in cmake for source in sources):
            errors.append(
                f"Receipt authority field {field} is marked only in unregistered "
                f"test source(s): {', '.join(sorted(sources))}"
            )
    return errors


def test_registration_errors(root: Path) -> list[str]:
    cmake_path = root / "tests/CMakeLists.txt"
    if not cmake_path.is_file():
        return ["tests/CMakeLists.txt is missing"]

    cmake = CMAKE_COMMENT_PATTERN.sub("", read_text(cmake_path))
    registered = set(
        re.findall(r"\bNAME\s+(test-[a-z0-9-]+)\b", cmake)
    )
    registered_contracts = set(
        re.findall(r"\bNAME\s+(contract-[a-z0-9-]+)\b", cmake)
    )
    errors: list[str] = []
    missing = sorted(REQUIRED_TEST_TARGETS - registered)
    if missing:
        errors.append(
            "required existing CTest targets were removed: " + ", ".join(missing)
        )
    missing_contracts = sorted(REQUIRED_CONTRACT_TARGETS - registered_contracts)
    if missing_contracts:
        errors.append(
            "required contract CTest targets were removed: "
            + ", ".join(missing_contracts)
        )
    if "function(cpp_add_contract_suite)" not in cmake:
        errors.append("the concrete contract CTest registration helper was removed")
    return errors


def schema_authority_errors(root: Path) -> list[str]:
    errors: list[str] = []
    for source_name, constant_name, schema_name in SCHEMA_AUTHORITIES:
        source_path = root / source_name
        schema_path = root / schema_name
        if not source_path.is_file() or not schema_path.is_file():
            continue
        pattern = re.compile(
            rf'{re.escape(constant_name)}\s*=\s*std::string_view\s*'
            rf'\{{\s*"([0-9a-f]{{64}})"\s*\}}'
        )
        match = pattern.search(read_text(source_path))
        if match is None:
            errors.append(f"{constant_name} has no single exact schema hash authority")
            continue
        actual = hashlib.sha256(schema_path.read_bytes()).hexdigest()
        if match.group(1) != actual:
            errors.append(
                f"{constant_name} does not match exact {schema_name} bytes"
            )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    arguments = parser.parse_args()
    root = arguments.root.resolve()

    errors = sorted(
        {
            *retired_path_errors(root),
            *cli_surface_errors(root),
            *business_global_errors(root),
            *retired_runtime_errors(root),
            *trusted_parser_errors(root),
            *generic_schema_errors(root),
            *receipt_validation_errors(root),
            *schema_authority_errors(root),
            *test_registration_errors(root),
        }
    )
    if errors:
        for error in errors:
            print(f"repository surface violation: {error}", file=sys.stderr)
        return 1

    print("repository surface: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
