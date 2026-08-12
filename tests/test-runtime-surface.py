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


# The command surface is two directories: entry/ owns main.cpp and nothing else,
# and modules/cli/ owns everything the commands are made of. Both are scanned,
# so a retired source cannot come back by being written to the other one.
CLI_SOURCE_ROOTS = ("entry", "modules/cli")
CLI_LIBRARY_SOURCE = "modules/cli/source/cli"

RETIRED_PATHS = (
    f"{CLI_LIBRARY_SOURCE}/check.cpp",
    f"{CLI_LIBRARY_SOURCE}/check.hpp",
    f"{CLI_LIBRARY_SOURCE}/file-frame-source.cpp",
    f"{CLI_LIBRARY_SOURCE}/file-frame-source.hpp",
    f"{CLI_LIBRARY_SOURCE}/platform/ocr-engine-binding-unsupported.cpp",
    f"{CLI_LIBRARY_SOURCE}/replay.cpp",
    f"{CLI_LIBRARY_SOURCE}/replay.hpp",
    f"{CLI_LIBRARY_SOURCE}/run.cpp",
    f"{CLI_LIBRARY_SOURCE}/run.hpp",
    f"{CLI_LIBRARY_SOURCE}/run-unsupported.cpp",
    f"{CLI_LIBRARY_SOURCE}/run-windows.cpp",
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
    f"{CLI_LIBRARY_SOURCE}/explore.cpp",
    f"{CLI_LIBRARY_SOURCE}/explore.hpp",
    f"{CLI_LIBRARY_SOURCE}/observe.cpp",
    f"{CLI_LIBRARY_SOURCE}/observe.hpp",
    f"{CLI_LIBRARY_SOURCE}/ocr.cpp",
    f"{CLI_LIBRARY_SOURCE}/ocr.hpp",
    f"{CLI_LIBRARY_SOURCE}/open-project.cpp",
    f"{CLI_LIBRARY_SOURCE}/open-project.hpp",
    f"{CLI_LIBRARY_SOURCE}/project-skeleton.cpp",
    f"{CLI_LIBRARY_SOURCE}/project-skeleton.hpp",
    f"{CLI_LIBRARY_SOURCE}/targets.cpp",
    f"{CLI_LIBRARY_SOURCE}/targets.hpp",
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
    "tests/cli/test-observe.cpp",
    "tests/cli/test-ocr.cpp",
    "tests/cli/test-open-project.cpp",
    "tests/cli/test-project-skeleton.cpp",
    "tests/cli/test-targets.cpp",
)

RETIRED_COMMANDS = frozenset({"check", "replay", "run"})
# open loads a project directory and registers its deployments' plugins. It
# reaches no target and drives no session, which is why it may share a binary
# whose whole invariant is acting on a real window. ocr is here on the same
# terms: it measures a PNG already on disk, opens no capture and posts no
# input, so it cannot reach a window at all.
#
# observe is the opposite case and belongs for the opposite reason: it binds a
# real window and takes a real capture, which is exactly what this binary is
# for. What it may not do is act, and that is a property of the composition
# rather than of the name -- it mints no Receipt, proposes no plan, and calls
# no verb of the action sink it builds. tests/cli/test-observe.cpp holds it to
# that against a recorded target that counts what it was asked to post.
ALLOWED_COMMANDS = frozenset({"explore", "observe", "ocr", "open", "targets"})
# Names that must not be bound in a project script's global table.
#
# WHAT THIS RULE READS AND WHAT IT DOES NOT. A project environment's globals are
# exactly three lists: the standard-library whitelist, `EngineConfig::
# projectGlobals` and `EngineConfig::frameworkProjectGlobals`
# (script/ffi/environment.cpp, installProjectEnvironmentPrototype). Every list
# is read below, from its own definition, in every environment -- and NOTHING
# ELSE IS. A global is a table, and this rule says nothing about that table's
# members: `key`, `drag`, `scroll`, `long_press` and `move_pointer` are methods
# of the cycle view `explore` hands out (modules/task/runtime/explore.luau) and
# are legal there, which is why they appear below and the gate is green. What
# the rule forbids is any of these names becoming a BINDING a project script can
# reach without going through the module that owns it.
#
# Three of these names are published on purpose, each by exactly one list, so
# each is allowed there by PUBLISHED_GLOBAL_AUTHORITIES and nowhere else. That
# is the property worth having: `explore` in the exploration environment is the
# authoring surface, and `explore` in any other list is business execution
# opening without an Operator.
FORBIDDEN_PROJECT_GLOBALS = frozenset(
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

# Every list whose contents become project globals, with the forbidden names
# that list alone may publish. The allowance is written here rather than read
# out of the source, so a name added to a whitelist is a failure rather than an
# agreement between the check and the thing it checks; an allowance the source
# stops exercising is a failure too, so a retired publication cannot leave a
# licence behind for the next one.
#
# `kind` picks the definition's spelling: "function" for the four whitelist
# functions, "array" for the standard-library table environment.cpp holds as a
# constant. Both are read from the single first-party definition of that name.
PUBLISHED_GLOBAL_AUTHORITIES = (
    ("k_projectStandardGlobals", "array", frozenset()),
    ("frameworkProjectGlobals", "function", frozenset()),
    ("scriptProjectGlobals", "function", frozenset()),
    ("explorationProjectGlobals", "function", frozenset({"explore"})),
    ("runtimeProjectGlobals", "function", frozenset({"observe", "project"})),
)

# The tokens a whitelist body may carry besides its own names. Anything else is
# an expression this rule cannot read, and it is reported rather than ignored:
# a whitelist that resolves its entries somewhere this gate cannot follow is a
# published surface nobody audits, which is the defect the rule exists for.
GLOBALS_BODY_SCAFFOLDING = frozenset(
    {"auto", "return", "std", "string", "string_view", "to_array", "vector"}
)
# The consumer's domain nouns, which a framework schema must not learn. "item"
# is deliberately absent: `items` is a JSON Schema keyword and `item_count` is
# the cardinality of the published Collection Fact, so the word carries the
# generic collection sense here. "inventory" names the consumer's sense of it.
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
REQUIRED_CHECK_TARGETS = frozenset({"check-repository-surface"})

# Every place a schema digest is pinned outside the schema file itself, in the
# spelling its own language uses.
#
# The Luau pin is here because it is the one nothing else could notice. A C++
# pin that drifts is compared inside the load path against a hash the same
# binary computed, so a mismatch is at least loud somewhere; a stale
# model.schema_hash is handed to the Host by the trusted parser and refuses
# every artifact at activation, in a lane no local gate exercises. Before this
# entry existed the gate printed PASS with that pin stale.
SCHEMA_AUTHORITIES = (
    (
        "modules/trace/source/trace/event.hpp",
        "k_traceSchemaHash",
        "schema/umbraflow-trace-v2.schema.json",
    ),
    (
        "modules/task/runtime/model.luau",
        "model.schema_hash",
        "schema/umbraflow-runtime-v2.schema.json",
    ),
    (
        "modules/task/source/task/runtime-model-file.hpp",
        "k_runtimeArtifactSchemaHash",
        "schema/umbraflow-runtime-artifact-v1.schema.json",
    ),
    (
        "modules/task/source/task/runtime-model-file.hpp",
        "k_runtimeModelSchemaHash",
        "schema/umbraflow-runtime-v2.schema.json",
    ),
    (
        "modules/operator/source/operator/runtime-installation.hpp",
        "k_annotationWorkspaceSchemaHash",
        "schema/umbraflow-annotation-workspace-v2.schema.json",
    ),
)

# One shape, written twice, because the boundary between the two writings may
# not be crossed by a file read.
#
# schema/umbraflow-runtime-v2.schema.json is the source: `state_readings` is
# what the trusted Luau resolver serializes, and those bytes are what
# k_runtimeModelSchemaHash and model.schema_hash pin. The Operator cannot read
# that file. k_commonSchema is a compiled constant precisely so a Host
# validating a plugin's derive input depends on no file a project could swap,
# so modules/deployment restates the same shape in the Operator's own $defs
# vocabulary, where a Reader id is `Identifier` rather than `identifier` and
# there is no $def for a reading or for a reason at all.
#
# This rule derives that restatement from the source and requires the embedded
# copy to be exactly it. Editing either side alone is red. Before it existed
# both had to be edited by hand -- 2cb070b did edit both -- and nothing would
# have noticed the half that was forgotten until every ProjectPlugin.derive
# call refused.
READINGS_SOURCE_SCHEMA = "schema/umbraflow-runtime-v2.schema.json"
READINGS_SOURCE_DEFINITION = "state_readings"
READINGS_EMBEDDED_SOURCE = (
    "modules/deployment/source/deployment/project-deployment.cpp"
)
READINGS_EMBEDDED_CONSTANT = "k_commonSchema"
READINGS_EMBEDDED_POINTER = (
    "$defs",
    "StateResolution",
    "properties",
    "readings",
)
# The whole of the difference the restatement is allowed to have. A definition
# the Operator names under another spelling is renamed; one it has no $def for
# is inlined. Every other $ref the source reaches is reported rather than
# passed through, because a reference this rule cannot restate is a part of the
# shape it would otherwise compare without having read it.
READINGS_DEFINITION_RENAMES = {"identifier": "Identifier"}
READINGS_INLINED_DEFINITIONS = frozenset(
    {"binding_reading", "reading_line", "rect", "unknown_reason"}
)
READINGS_ABSENT = object()
EMBEDDED_SCHEMA_LITERAL_PATTERN = re.compile(
    rf"{re.escape(READINGS_EMBEDDED_CONSTANT)}\s*=\s*std::string_view\s*\{{\s*"
    r'R"json\((.*?)\)json"\s*\}',
    re.DOTALL,
)

COMMAND_PATTERN = re.compile(r'Command\s*\{\s*"([a-z0-9-]+)"')
STRING_LITERAL_PATTERN = re.compile(r'"([^"\\]*(?:\\.[^"\\]*)*)"')
STRING_CONSTANT_PATTERN = re.compile(
    r"\b(?:constexpr\s+)?(?:auto|char\s+(?:const\s+)?)\s+"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*=\s*\"([^\"\\]*)\""
)
# Where a VM is handed a published-globals list. The rule reads the boot sites
# as well as the whitelists, because a list is only one of the two ways a name
# reaches a project environment: the other is an initializer written inline at
# the site, which no whitelist function would ever carry.
PUBLICATION_MEMBER_PATTERN = re.compile(
    r"\.(projectGlobals|frameworkProjectGlobals)\s*=\s*"
)
IDENTIFIER_PATTERN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
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
RUNTIME_MODEL_PARSER_SOURCE = "modules/task/runtime/project.luau"
RUNTIME_MODEL_SCHEMA = "schema/umbraflow-runtime-v2.schema.json"

# The declaration of the Snapshot Coordinator's entry point, in the one header
# that declares it. The rule below reads its parameter list.
CREATE_SNAPSHOT_DECLARATION_PATTERN = re.compile(
    r"auto\s+createSnapshot\s*\(([^)]*)\)",
    re.DOTALL,
)

# A caller-supplied snapshot identity, by any of the spellings it has had. The
# hash is derived inside the publishing transaction from what that transaction
# read, so a parameter carrying one is the whole defect s02 exists to close and
# it cannot come back under a different case.
CALLER_SNAPSHOT_IDENTITY_PATTERN = re.compile(
    r"[A-Za-z0-9_]*(?:identityHash|IdentityHash|identity_hash)"
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

    cli_paths = sorted(
        path
        for source_root in CLI_SOURCE_ROOTS
        for path in (root / source_root).rglob("*")
    )
    for path in cli_paths:
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


def authority_definition_pattern(name: str, kind: str) -> re.Pattern[str]:
    if kind == "array":
        return re.compile(
            rf"\bconstexpr\s+auto\s+{re.escape(name)}\s*=\s*"
            rf"std::to_array<std::string_view>\s*\(\s*\{{"
        )
    return re.compile(
        rf"\bauto\s+{re.escape(name)}\s*\(\s*\)\s*(?:noexcept\s*)?->[^;{{]+\{{"
    )


def read_published_names(source: str, body: str) -> tuple[set[str], set[str]]:
    """The names a whitelist body publishes, and the tokens it could not read.

    A string constant declared anywhere in the same translation unit and named
    in the body counts as published under its value, so a whitelist spelling its
    entry as a constant is read exactly like one spelling it inline.
    """
    published = {
        match.group(1).lower() for match in STRING_LITERAL_PATTERN.finditer(body)
    }
    residue = STRING_LITERAL_PATTERN.sub(" ", body)
    for match in STRING_CONSTANT_PATTERN.finditer(source):
        name = match.group(1)
        if re.search(rf"\b{re.escape(name)}\b", residue) is None:
            continue
        published.add(match.group(2).lower())
        residue = re.sub(rf"\b{re.escape(name)}\b", " ", residue)
    unread = {
        token
        for token in IDENTIFIER_PATTERN.findall(residue)
        if token not in GLOBALS_BODY_SCAFFOLDING
    }
    return published, unread


def read_initializer(text: str, start: int) -> str:
    """The initializer expression beginning at `start`, up to its own comma."""
    depth = 0
    index = start
    while index < len(text):
        character = text[index]
        if character == '"':
            match = STRING_LITERAL_PATTERN.match(text, index)
            index = index + 1 if match is None else match.end()
            continue
        if character in "({[":
            depth += 1
        elif character in ")}]":
            if depth == 0:
                return text[start:index]
            depth -= 1
        elif character == "," and depth == 0:
            return text[start:index]
        index += 1
    return text[start:]


def published_global_errors(root: Path) -> list[str]:
    errors: list[str] = []
    authority_names = {name for name, _, _ in PUBLISHED_GLOBAL_AUTHORITIES}
    for name, kind, allowed in PUBLISHED_GLOBAL_AUTHORITIES:
        pattern = authority_definition_pattern(name, kind)
        definitions: list[tuple[Path, str, str]] = []
        for path in first_party_sources(root):
            if path.suffix.lower() not in {".cpp", ".cc", ".c"}:
                continue
            text = CPP_COMMENT_PATTERN.sub("", read_text(path))
            for match in pattern.finditer(text):
                body = extract_braced_body(text, match.end() - 1)
                if body is not None:
                    definitions.append((path, body, text))

        if len(definitions) != 1:
            errors.append(
                f"{name} must have exactly one first-party definition this rule "
                f"can read; found {len(definitions)}"
            )
            continue

        path, body, source = definitions[0]
        relative = path.relative_to(root).as_posix()
        published, unread = read_published_names(source, body)
        if unread:
            errors.append(
                f"{relative}: {name} resolves its entries through "
                f"{', '.join(sorted(unread))}, which this rule cannot read, so "
                "what it publishes is unaudited"
            )
        forbidden = sorted((published & FORBIDDEN_PROJECT_GLOBALS) - allowed)
        if forbidden:
            errors.append(
                f"{relative}: {name} publishes privileged or direct-action "
                f"project globals: {', '.join(forbidden)}"
            )
        stale = sorted(allowed - published)
        if stale:
            errors.append(
                f"{name} no longer publishes {', '.join(stale)}, so its "
                "allowance in PUBLISHED_GLOBAL_AUTHORITIES licenses a name "
                "nothing publishes"
            )

    # The boot sites. Without them a new environment could publish a forbidden
    # name in an initializer written where no whitelist function can see it.
    sites = {"projectGlobals": 0, "frameworkProjectGlobals": 0}
    for path in first_party_sources(root):
        if path.suffix.lower() not in {".cpp", ".cc", ".c", ".h", ".hpp"}:
            continue
        text = CPP_COMMENT_PATTERN.sub("", read_text(path))
        relative = path.relative_to(root).as_posix()
        for match in PUBLICATION_MEMBER_PATTERN.finditer(text):
            sites[match.group(1)] += 1
            initializer = read_initializer(text, match.end())
            literals, unread = read_published_names(source="", body=initializer)
            if unread & authority_names:
                continue
            if unread:
                errors.append(
                    f"{relative}: .{match.group(1)} is initialized from "
                    f"{', '.join(sorted(unread))}, which is neither a whitelist "
                    "this rule reads nor a list of names it can see"
                )
                continue
            forbidden = sorted(literals & FORBIDDEN_PROJECT_GLOBALS)
            if forbidden:
                errors.append(
                    f"{relative}: .{match.group(1)} publishes privileged or "
                    f"direct-action project globals: {', '.join(forbidden)}"
                )
    errors.extend(
        f"no first-party source assigns .{member}, so the publication rule read "
        "no environment at all"
        for member, count in sorted(sites.items())
        if count == 0
    )
    return errors


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

        if "RuntimeModel" in original or "runtime-model.toml" in original:
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


def reader_member_parity_errors(root: Path) -> list[str]:
    parser_path = root / RUNTIME_MODEL_PARSER_SOURCE
    schema_path = root / RUNTIME_MODEL_SCHEMA
    if not parser_path.is_file() or not schema_path.is_file():
        return [
            "RuntimeModel Reader member parity cannot find both the trusted "
            "parser and published schema"
        ]

    parser_text = executable_text(parser_path, read_text(parser_path))
    section_fields = re.search(
        r"\blocal\s+sectionFields\b[^=]*=\s*\{",
        parser_text,
    )
    if section_fields is None:
        return [
            "RuntimeModel Reader member parity cannot read the trusted "
            "parser's sectionFields table"
        ]
    section_body = extract_braced_body(parser_text, section_fields.end() - 1)
    if section_body is None:
        return [
            "RuntimeModel Reader member parity found an unterminated trusted "
            "parser sectionFields table"
        ]

    reader_fields = re.search(r"\breader\s*=\s*\{", section_body)
    if reader_fields is None:
        return [
            "RuntimeModel Reader member parity cannot read trusted parser "
            "sectionFields.reader"
        ]
    reader_body = extract_braced_body(section_body, reader_fields.end() - 1)
    if reader_body is None:
        return [
            "RuntimeModel Reader member parity found an unterminated trusted "
            "parser sectionFields.reader table"
        ]
    parser_members = set(
        re.findall(r"\b([a-z][a-z0-9_]*)\s*=\s*true\b", reader_body)
    )
    if not parser_members:
        return [
            "RuntimeModel Reader member parity found no trusted parser members"
        ]

    try:
        schema = json.loads(schema_path.read_bytes())
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"RuntimeModel Reader member parity found invalid schema: {error}"]
    definitions = schema.get("$defs") if isinstance(schema, dict) else None
    reader_schema = definitions.get("reader") if isinstance(definitions, dict) else None
    properties = reader_schema.get("properties") if isinstance(reader_schema, dict) else None
    if not isinstance(properties, dict):
        return [
            "RuntimeModel Reader member parity cannot read published schema "
            "$defs.reader.properties"
        ]

    schema_members = set(properties)
    parser_only = sorted(parser_members - schema_members)
    schema_only = sorted(schema_members - parser_members)
    if not parser_only and not schema_only:
        return []

    differences = []
    if parser_only:
        differences.append("trusted parser only: " + ", ".join(parser_only))
    if schema_only:
        differences.append("published schema only: " + ", ".join(schema_only))
    return ["RuntimeModel Reader member parity differs: " + "; ".join(differences)]


def snapshot_identity_errors(root: Path) -> list[str]:
    """createSnapshot must not accept a snapshot identity from its caller.

    The gate reads a declaration rather than a call, because a caller that can
    name the identity can pin a snapshot to a world the ledger never held, and
    that is a property of the signature. It fails loudly when the declaration
    cannot be found at all: a rule that silently matches nothing is the class of
    check this repository has already shipped once.
    """
    header = root / "modules/operator/source/operator/ledger.hpp"
    if not header.is_file():
        return ["modules/operator/source/operator/ledger.hpp is missing"]

    text = executable_text(header, read_text(header))
    match = CREATE_SNAPSHOT_DECLARATION_PATTERN.search(text)
    if match is None:
        return [
            "modules/operator/source/operator/ledger.hpp declares no "
            "createSnapshot for the snapshot-identity rule to read"
        ]

    offender = CALLER_SNAPSHOT_IDENTITY_PATTERN.search(match.group(1))
    if offender is not None:
        return [
            "modules/operator/source/operator/ledger.hpp: createSnapshot takes a "
            f"caller-supplied snapshot identity {offender.group(0)!r}"
        ]
    return []


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
    registered_checks = set(
        re.findall(r"\bNAME\s+(check-[a-z0-9-]+)\b", cmake)
    )
    errors: list[str] = []
    missing = sorted(REQUIRED_TEST_TARGETS - registered)
    if missing:
        errors.append(
            "required existing CTest targets were removed: " + ", ".join(missing)
        )
    missing_checks = sorted(REQUIRED_CHECK_TARGETS - registered_checks)
    if missing_checks:
        errors.append(
            "required check CTest targets were removed: " + ", ".join(missing_checks)
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
        # One rule, two spellings, because the pin is written in two languages.
        # A Luau assignment has no std::string_view around it, and reading it
        # with the C++ pattern is how a pin can be present, wrong, and reported
        # as absent -- or, before this branch existed, not read at all.
        pattern = (
            re.compile(rf'{re.escape(constant_name)}\s*=\s*"([0-9a-f]{{64}})"')
            if source_path.suffix == ".luau"
            else re.compile(
                rf'{re.escape(constant_name)}\s*=\s*std::string_view\s*'
                rf'\{{\s*"([0-9a-f]{{64}})"\s*\}}'
            )
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


def project_readings(
    node: Any,
    definitions: dict[str, Any],
    unreadable: set[str],
) -> Any:
    """The source shape, restated in the Operator's $defs vocabulary."""
    if isinstance(node, dict):
        reference = node.get("$ref")
        if isinstance(reference, str):
            name = reference.removeprefix("#/$defs/")
            if name == reference or name not in definitions:
                unreadable.add(reference)
                return node
            if name in READINGS_INLINED_DEFINITIONS:
                return project_readings(definitions[name], definitions, unreadable)
            renamed = READINGS_DEFINITION_RENAMES.get(name)
            if renamed is None:
                unreadable.add(reference)
                return node
            return {"$ref": f"#/$defs/{renamed}"}
        return {
            key: project_readings(value, definitions, unreadable)
            for key, value in node.items()
            if key != "$comment"
        }
    if isinstance(node, list):
        return [project_readings(value, definitions, unreadable) for value in node]
    return node


def without_comments(node: Any) -> Any:
    """The same document with its prose removed.

    Each side argues for the shape in its own words and to its own reader, and
    neither is the contract. Everything else is.
    """
    if isinstance(node, dict):
        return {
            key: without_comments(value)
            for key, value in node.items()
            if key != "$comment"
        }
    if isinstance(node, list):
        return [without_comments(value) for value in node]
    return node


def first_difference(
    expected: Any,
    actual: Any,
    pointer: str = "",
) -> tuple[str, Any, Any] | None:
    """Where two documents first disagree, as a pointer and both values."""
    if isinstance(expected, dict) and isinstance(actual, dict):
        for key in sorted(set(expected) | set(actual)):
            at = f"{pointer}/{key}"
            if key not in expected or key not in actual:
                return (
                    at,
                    expected.get(key, READINGS_ABSENT),
                    actual.get(key, READINGS_ABSENT),
                )
            found = first_difference(expected[key], actual[key], at)
            if found is not None:
                return found
        return None
    if isinstance(expected, list) and isinstance(actual, list):
        for index in range(max(len(expected), len(actual))):
            at = f"{pointer}/{index}"
            if index >= len(expected) or index >= len(actual):
                return (
                    at,
                    expected[index] if index < len(expected) else READINGS_ABSENT,
                    actual[index] if index < len(actual) else READINGS_ABSENT,
                )
            found = first_difference(expected[index], actual[index], at)
            if found is not None:
                return found
        return None
    if expected != actual:
        return (pointer, expected, actual)
    return None


def render_difference(value: Any) -> str:
    if value is READINGS_ABSENT:
        return "nothing"
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def readings_contract_errors(root: Path) -> list[str]:
    """The embedded readings shape must be the published one, restated.

    Every failure below is loud, including the ones that mean this rule found
    nothing to read. A shape check that silently compares two documents it
    never opened reports agreement it did not establish, which is worse than
    the duplication it exists to guard.
    """
    schema_path = root / READINGS_SOURCE_SCHEMA
    source_path = root / READINGS_EMBEDDED_SOURCE
    if not schema_path.is_file():
        return [
            f"{READINGS_SOURCE_SCHEMA} is missing; the readings shape has no "
            "source"
        ]
    if not source_path.is_file():
        return [
            f"{READINGS_EMBEDDED_SOURCE} is missing; the readings shape has no "
            "embedded restatement"
        ]

    try:
        document = json.loads(schema_path.read_bytes())
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"{READINGS_SOURCE_SCHEMA} is invalid: {error}"]
    definitions = document.get("$defs")
    if (
        not isinstance(definitions, dict)
        or READINGS_SOURCE_DEFINITION not in definitions
    ):
        return [
            f"{READINGS_SOURCE_SCHEMA} declares no "
            f"$defs/{READINGS_SOURCE_DEFINITION}, which is the source of the "
            "readings shape"
        ]

    match = EMBEDDED_SCHEMA_LITERAL_PATTERN.search(read_text(source_path))
    if match is None:
        return [
            f"{READINGS_EMBEDDED_SOURCE} declares no {READINGS_EMBEDDED_CONSTANT} "
            "raw JSON literal for the readings rule to read"
        ]
    try:
        embedded = json.loads(match.group(1))
    except json.JSONDecodeError as error:
        return [
            f"{READINGS_EMBEDDED_SOURCE}: {READINGS_EMBEDDED_CONSTANT} is not "
            f"JSON: {error}"
        ]
    for key in READINGS_EMBEDDED_POINTER:
        if not isinstance(embedded, dict) or key not in embedded:
            return [
                f"{READINGS_EMBEDDED_SOURCE}: {READINGS_EMBEDDED_CONSTANT} carries "
                "no " + "/".join(READINGS_EMBEDDED_POINTER) + " to compare"
            ]
        embedded = embedded[key]

    unreadable: set[str] = set()
    expected = project_readings(
        definitions[READINGS_SOURCE_DEFINITION],
        definitions,
        unreadable,
    )
    if unreadable:
        return [
            f"{READINGS_SOURCE_SCHEMA}: {READINGS_SOURCE_DEFINITION} reaches "
            + ", ".join(sorted(unreadable))
            + ", which this rule cannot restate in the Operator's vocabulary, so "
            "part of the shape would be compared unread"
        ]

    difference = first_difference(expected, without_comments(embedded))
    if difference is None:
        return []
    pointer, from_source, from_embedded = difference
    return [
        f"{READINGS_EMBEDDED_SOURCE}: {READINGS_EMBEDDED_CONSTANT} restates "
        f"{READINGS_SOURCE_SCHEMA}'s {READINGS_SOURCE_DEFINITION}, and the two "
        f"disagree at {pointer or '/'}: the schema states "
        f"{render_difference(from_source)} and the embedded copy carries "
        f"{render_difference(from_embedded)}"
    ]


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
            *published_global_errors(root),
            *retired_runtime_errors(root),
            *trusted_parser_errors(root),
            *reader_member_parity_errors(root),
            *snapshot_identity_errors(root),
            *generic_schema_errors(root),
            *readings_contract_errors(root),
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
