#!/usr/bin/env python3
"""Migrate one exact format-4 ProjectRegistration to the two-closure format.

The production directory loader currently derives a ProjectRegistration in
memory; it does not give that document a filename. The caller therefore names
the exact registration document to migrate, along with every logical module
name and project-relative file path in each replacement closure. The caller
also supplies each closure's authored export statement as a JSON array; an
empty tool export set is therefore the explicit value ``[]``. Nothing here
infers a logical module name from a host filename or an export set from the
binding table.

Normal mode performs exactly one 4-to-5 rewrite and refuses an already migrated
document. ``--check`` never writes: it exits non-zero for format 4 and reports
the two closures it would emit, while a matching format-5 document passes.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import stat
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import NoReturn


FORMAT_MEMBER = "project_registration_format"
FORMAT_4 = 4
FORMAT_5 = 5

HASH_PATTERN = re.compile(r"^[0-9a-f]{64}$")
MODULE_NAME_PATTERN = re.compile(
    r"^[a-z][a-z0-9_-]{0,63}(/[a-z][a-z0-9_-]{0,63}){0,15}$"
)
NAMESPACED_NAME_PATTERN = re.compile(
    r"^[a-z][a-z0-9_-]*(\.[a-z][a-z0-9_-]*)+$"
)
ROOT_NAME_PATTERN = re.compile(
    r"^[a-z][a-z0-9_-]{0,63}(\.[a-z][a-z0-9_-]{0,63}){0,15}$"
)

COMMON_HASH_MEMBERS = {
    "journal_event_schema_manifest_hash",
    "plugin_environment_hash",
    "project_observation_schema_hash",
    "project_state_schema_hash",
    "project_tool_precondition_schema_hash",
    "reconcile_payload_schema_manifest_hash",
    "tool_catalog_hash",
}
COMMON_MEMBERS = {
    FORMAT_MEMBER,
    "baseline_event_type",
    "observed_instance_identity_schema_hashes",
    "plugin_id",
    "project_resources",
    "project_tool_bindings",
    *COMMON_HASH_MEMBERS,
}
FORMAT_4_MEMBERS = {*COMMON_MEMBERS, "plugin_module_manifest_hash"}
FORMAT_5_MEMBERS = {*COMMON_MEMBERS, "reducer_closure", "tool_closure"}
CLOSURE_MEMBERS = {"exported_entry_points", "module_manifest_hash"}


class MigrationError(ValueError):
    """A refusal that should be reported without a Python traceback."""


@dataclass(frozen=True)
class ModuleSource:
    name: str
    path: Path
    sha256: str


@dataclass(frozen=True)
class ClosureSource:
    entry: str
    modules: tuple[ModuleSource, ...]
    manifest_hash: str


def refuse(message: str) -> NoReturn:
    raise MigrationError(message)


def unique_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for name, value in pairs:
        if name in result:
            refuse(f"JSON object member appears more than once: {name}")
        result[name] = value
    return result


def reject_non_json_number(value: str) -> NoReturn:
    refuse(f"registration contains a non-JSON number: {value}")


def canonical_json_bytes(value: object) -> bytes:
    """Render JCS for this contract's integer-and-ASCII-key value domain."""
    return json.dumps(
        value,
        allow_nan=False,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def read_json(path: Path) -> tuple[dict[str, object], bytes]:
    exact_bytes = path.read_bytes()
    try:
        text = exact_bytes.decode("utf-8")
        document = json.loads(
            text,
            object_pairs_hook=unique_object,
            parse_constant=reject_non_json_number,
        )
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        refuse(f"{path} is not exact UTF-8 JSON: {error}")
    if not isinstance(document, dict):
        refuse(f"{path} must contain a JSON object")
    return document, exact_bytes


def require_exact_members(
    value: object,
    expected: set[str],
    what: str,
) -> dict[str, object]:
    if not isinstance(value, dict):
        refuse(f"{what} must be an object")
    actual = set(value)
    missing = sorted(expected - actual)
    unknown = sorted(actual - expected)
    if missing:
        refuse(f"{what} is missing required member(s): {', '.join(missing)}")
    if unknown:
        refuse(f"{what} has unknown member(s): {', '.join(unknown)}")
    return value


def require_string(value: object, what: str) -> str:
    if not isinstance(value, str):
        refuse(f"{what} must be a string")
    return value


def require_pattern(value: object, pattern: re.Pattern[str], what: str) -> str:
    text = require_string(value, what)
    if pattern.fullmatch(text) is None:
        refuse(f"{what} has an invalid spelling: {text!r}")
    return text


def require_array(value: object, what: str, maximum: int | None = None) -> list[object]:
    if not isinstance(value, list):
        refuse(f"{what} must be an array")
    if maximum is not None and len(value) > maximum:
        refuse(f"{what} exceeds its {maximum}-item ceiling")
    return value


def validate_hash(value: object, what: str) -> str:
    return require_pattern(value, HASH_PATTERN, what)


def validate_resources(value: object) -> None:
    rows = require_array(value, "project_resources", 64)
    names: list[str] = []
    for index, row_value in enumerate(rows):
        what = f"project_resources[{index}]"
        row = require_exact_members(
            row_value,
            {"kind", "name", "sha256", "size"},
            what,
        )
        kind = require_string(row["kind"], f"{what}.kind")
        if kind not in {"bytes", "json", "utf8"}:
            refuse(f"{what}.kind has an invalid value: {kind!r}")
        names.append(require_pattern(row["name"], ROOT_NAME_PATTERN, f"{what}.name"))
        validate_hash(row["sha256"], f"{what}.sha256")
        size = row["size"]
        if type(size) is not int or not 0 <= size <= 4_194_304:
            refuse(f"{what}.size must be an integer from 0 through 4194304")
    if names != sorted(names) or len(names) != len(set(names)):
        refuse("project_resources must be unique and JCS-ordered by name")


def validate_bindings(value: object) -> list[dict[str, object]]:
    rows = require_array(value, "project_tool_bindings", 256)
    bindings: list[dict[str, object]] = []
    tool_names: list[str] = []
    for index, row_value in enumerate(rows):
        what = f"project_tool_bindings[{index}]"
        row = require_exact_members(
            row_value,
            {"entry_point", "tool_name"},
            what,
        )
        require_pattern(row["entry_point"], ROOT_NAME_PATTERN, f"{what}.entry_point")
        tool_names.append(
            require_pattern(
                row["tool_name"],
                NAMESPACED_NAME_PATTERN,
                f"{what}.tool_name",
            )
        )
        bindings.append(row)
    if tool_names != sorted(tool_names) or len(tool_names) != len(set(tool_names)):
        refuse("project_tool_bindings must be unique and JCS-ordered by tool_name")
    return bindings


def validate_common(document: dict[str, object]) -> list[dict[str, object]]:
    require_pattern(document["plugin_id"], NAMESPACED_NAME_PATTERN, "plugin_id")
    require_pattern(
        document["baseline_event_type"],
        NAMESPACED_NAME_PATTERN,
        "baseline_event_type",
    )
    for name in sorted(COMMON_HASH_MEMBERS):
        validate_hash(document[name], name)

    identity_hashes = require_array(
        document["observed_instance_identity_schema_hashes"],
        "observed_instance_identity_schema_hashes",
    )
    parsed_identity_hashes = [
        validate_hash(value, f"observed_instance_identity_schema_hashes[{index}]")
        for index, value in enumerate(identity_hashes)
    ]
    if parsed_identity_hashes != sorted(set(parsed_identity_hashes)):
        refuse(
            "observed_instance_identity_schema_hashes must be unique and "
            "JCS-ordered"
        )

    validate_resources(document["project_resources"])
    return validate_bindings(document["project_tool_bindings"])


def validate_format_4(document: dict[str, object]) -> list[dict[str, object]]:
    require_exact_members(document, FORMAT_4_MEMBERS, "format-4 registration")
    validate_hash(
        document["plugin_module_manifest_hash"],
        "plugin_module_manifest_hash",
    )
    return validate_common(document)


def exported_tool_entries(bindings: list[dict[str, object]]) -> list[str]:
    return sorted({str(binding["entry_point"]) for binding in bindings})


def parse_exported_entries(value: str, what: str) -> list[str]:
    try:
        parsed = json.loads(value, parse_constant=reject_non_json_number)
    except json.JSONDecodeError as error:
        refuse(f"{what} must be a JSON array: {error}")
    entries = require_array(parsed, what, 256)
    result = [
        require_pattern(entry, ROOT_NAME_PATTERN, f"{what}[{index}]")
        for index, entry in enumerate(entries)
    ]
    if result != sorted(set(result)):
        refuse(f"{what} must be unique and JCS-ordered")
    return result


def validate_closure(
    value: object,
    what: str,
    expected_entries: list[str],
) -> dict[str, object]:
    closure = require_exact_members(value, CLOSURE_MEMBERS, what)
    validate_hash(closure["module_manifest_hash"], f"{what}.module_manifest_hash")
    entries = require_array(closure["exported_entry_points"], f"{what}.exported_entry_points", 256)
    parsed = [
        require_pattern(entry, ROOT_NAME_PATTERN, f"{what}.exported_entry_points[{index}]")
        for index, entry in enumerate(entries)
    ]
    if parsed != sorted(set(parsed)):
        refuse(f"{what}.exported_entry_points must be unique and JCS-ordered")
    if parsed != expected_entries:
        refuse(
            f"{what}.exported_entry_points is {parsed!r}; expected "
            f"{expected_entries!r}"
        )
    return closure


def validate_format_5(document: dict[str, object]) -> None:
    require_exact_members(document, FORMAT_5_MEMBERS, "format-5 registration")
    bindings = validate_common(document)
    validate_closure(document["reducer_closure"], "reducer_closure", ["reduce"])
    validate_closure(
        document["tool_closure"],
        "tool_closure",
        exported_tool_entries(bindings),
    )


def confined_file(root: Path, relative: str, what: str) -> Path:
    supplied = Path(relative)
    if supplied.is_absolute():
        refuse(f"{what} must be relative to the project directory: {relative}")
    try:
        resolved = (root / supplied).resolve(strict=True)
    except OSError as error:
        refuse(f"{what} does not name a readable file: {relative}: {error}")
    try:
        resolved.relative_to(root)
    except ValueError:
        refuse(f"{what} leaves the project directory: {relative}")
    if not resolved.is_file():
        refuse(f"{what} is not a file: {relative}")
    return resolved


def closure_source(
    root: Path,
    entry: str,
    declarations: list[str],
    what: str,
) -> ClosureSource:
    require_pattern(entry, MODULE_NAME_PATTERN, f"{what} entry module")
    modules: list[ModuleSource] = []
    names: set[str] = set()
    paths: set[Path] = set()
    for declaration in declarations:
        if "=" not in declaration:
            refuse(
                f"{what} module must use LOGICAL_NAME=PROJECT_RELATIVE_PATH: "
                f"{declaration!r}"
            )
        name, relative = declaration.split("=", 1)
        require_pattern(name, MODULE_NAME_PATTERN, f"{what} logical module name")
        if not relative:
            refuse(f"{what} module {name!r} has no project-relative path")
        if name in names:
            refuse(f"{what} logical module name appears more than once: {name}")
        path = confined_file(root, relative, f"{what} module {name}")
        if path in paths:
            refuse(f"{what} module file appears more than once: {relative}")
        names.add(name)
        paths.add(path)
        modules.append(
            ModuleSource(
                name=name,
                path=path,
                sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
            )
        )
    if entry not in names:
        refuse(f"{what} entry module {entry!r} is not present in its closure")

    modules.sort(key=lambda module: module.name)
    manifest = {
        "entry": entry,
        "modules": [
            {"name": module.name, "sha256": module.sha256}
            for module in modules
        ],
    }
    manifest_hash = hashlib.sha256(canonical_json_bytes(manifest)).hexdigest()
    return ClosureSource(
        entry=entry,
        modules=tuple(modules),
        manifest_hash=manifest_hash,
    )


def closure_value(source: ClosureSource, entries: list[str]) -> dict[str, object]:
    return {
        "exported_entry_points": entries,
        "module_manifest_hash": source.manifest_hash,
    }


def migrated_document(
    current: dict[str, object],
    reducer: ClosureSource,
    reducer_entries: list[str],
    tool: ClosureSource,
    tool_entries: list[str],
) -> dict[str, object]:
    migrated = dict(current)
    del migrated["plugin_module_manifest_hash"]
    migrated[FORMAT_MEMBER] = FORMAT_5
    migrated["reducer_closure"] = closure_value(reducer, reducer_entries)
    migrated["tool_closure"] = closure_value(tool, tool_entries)
    return migrated


def write_atomically(path: Path, exact_bytes: bytes) -> None:
    descriptor, temporary_name = tempfile.mkstemp(
        dir=path.parent,
        prefix=f".{path.name}.",
        suffix=".tmp",
    )
    temporary = Path(temporary_name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(exact_bytes)
            stream.flush()
            os.fsync(stream.fileno())
        temporary.chmod(stat.S_IMODE(path.stat().st_mode))
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def print_plan(
    action: str,
    registration: Path,
    root: Path,
    reducer: ClosureSource,
    reducer_entries: list[str],
    tool: ClosureSource,
    tool_entries: list[str],
) -> None:
    display = registration.relative_to(root).as_posix()
    print(f"{action}: {display}")
    print(f"  {FORMAT_MEMBER}: {FORMAT_4} -> {FORMAT_5}")
    print(
        "  reducer_closure: "
        f"module_manifest_hash={reducer.manifest_hash}, "
        "exported_entry_points="
        + json.dumps(reducer_entries, separators=(",", ":"))
    )
    print(
        "  tool_closure: "
        f"module_manifest_hash={tool.manifest_hash}, "
        "exported_entry_points="
        + json.dumps(tool_entries, separators=(",", ":"))
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("project_directory", type=Path)
    parser.add_argument(
        "--registration",
        required=True,
        help="project-relative path to the exact ProjectRegistration JSON",
    )
    parser.add_argument("--reducer-entry", required=True, help="reducer entry module name")
    parser.add_argument(
        "--reducer-module",
        action="append",
        required=True,
        metavar="NAME=PATH",
        help="logical reducer module name and project-relative source path; repeatable",
    )
    parser.add_argument(
        "--reducer-exported-entry-points",
        required=True,
        metavar="JSON",
        help='authored reducer export array; must be ["reduce"]',
    )
    parser.add_argument("--tool-entry", required=True, help="tool entry module name")
    parser.add_argument(
        "--tool-module",
        action="append",
        required=True,
        metavar="NAME=PATH",
        help="logical tool module name and project-relative source path; repeatable",
    )
    parser.add_argument(
        "--tool-exported-entry-points",
        required=True,
        metavar="JSON",
        help="authored tool export array; use [] for an explicitly empty set",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="report a required migration without writing and fail until complete",
    )
    return parser.parse_args()


def run(arguments: argparse.Namespace) -> int:
    try:
        root = arguments.project_directory.resolve(strict=True)
    except OSError as error:
        refuse(f"project directory does not exist: {arguments.project_directory}: {error}")
    if not root.is_dir():
        refuse(f"project directory is not a directory: {arguments.project_directory}")

    registration = confined_file(root, arguments.registration, "registration")
    document, exact_bytes = read_json(registration)
    if FORMAT_MEMBER not in document:
        refuse(
            f"{registration} has no {FORMAT_MEMBER}; this migration accepts "
            "an explicit format-4 ProjectRegistration and does not guess from shape"
        )
    format_value = document[FORMAT_MEMBER]
    if type(format_value) is not int:
        refuse(f"{registration} has a non-integer {FORMAT_MEMBER}: {format_value!r}")

    if format_value == FORMAT_5 and not arguments.check:
        refuse(
            f"{registration} already states {FORMAT_MEMBER} {FORMAT_5}; "
            "refusing to migrate it twice"
        )
    if format_value not in {FORMAT_4, FORMAT_5}:
        refuse(
            f"{registration} states {FORMAT_MEMBER} {format_value}; "
            f"this migration accepts exactly {FORMAT_4}"
        )

    reducer_entries = parse_exported_entries(
        arguments.reducer_exported_entry_points,
        "reducer exported_entry_points",
    )
    if reducer_entries != ["reduce"]:
        refuse(
            "reducer exported_entry_points must be exactly [\"reduce\"] for "
            "project_registration_format 5"
        )
    stated_tool_entries = parse_exported_entries(
        arguments.tool_exported_entry_points,
        "tool exported_entry_points",
    )

    reducer = closure_source(
        root,
        arguments.reducer_entry,
        arguments.reducer_module,
        "reducer closure",
    )
    tool = closure_source(
        root,
        arguments.tool_entry,
        arguments.tool_module,
        "tool closure",
    )
    reducer_entry_path = next(
        module.path for module in reducer.modules if module.name == reducer.entry
    )
    tool_entry_path = next(module.path for module in tool.modules if module.name == tool.entry)
    if reducer_entry_path == tool_entry_path:
        refuse(
            "reducer and tool entry modules resolve to the same file; the two "
            "program types require distinct exported sets"
        )

    if format_value == FORMAT_4:
        bindings = validate_format_4(document)
        if exact_bytes != canonical_json_bytes(document):
            refuse(
                f"{registration} is not exact RFC 8785 JCS; refusing to rewrite "
                "bytes the format-4 reader would not accept"
            )
        bound_tool_entries = exported_tool_entries(bindings)
        if stated_tool_entries != bound_tool_entries:
            refuse(
                "tool exported_entry_points does not equal the sorted, unique "
                f"project_tool_bindings union: stated {stated_tool_entries!r}, "
                f"bindings require {bound_tool_entries!r}"
            )
        migrated = migrated_document(
            document,
            reducer,
            reducer_entries,
            tool,
            stated_tool_entries,
        )
        validate_format_5(migrated)
        rendered = canonical_json_bytes(migrated)
        if arguments.check:
            print_plan(
                "Would migrate",
                registration,
                root,
                reducer,
                reducer_entries,
                tool,
                stated_tool_entries,
            )
            print(
                "ProjectRegistration migration required; run without --check.",
                file=sys.stderr,
            )
            return 1
        write_atomically(registration, rendered)
        print_plan(
            "Migrated",
            registration,
            root,
            reducer,
            reducer_entries,
            tool,
            stated_tool_entries,
        )
        return 0

    validate_format_5(document)
    if exact_bytes != canonical_json_bytes(document):
        refuse(f"{registration} is not exact RFC 8785 JCS")
    bindings = validate_bindings(document["project_tool_bindings"])
    bound_tool_entries = exported_tool_entries(bindings)
    if stated_tool_entries != bound_tool_entries:
        refuse(
            "tool exported_entry_points does not equal the sorted, unique "
            f"project_tool_bindings union: stated {stated_tool_entries!r}, "
            f"bindings require {bound_tool_entries!r}"
        )
    expected_reducer = closure_value(reducer, reducer_entries)
    expected_tool = closure_value(tool, stated_tool_entries)
    if document["reducer_closure"] != expected_reducer:
        refuse(
            f"{registration} is format 5, but reducer_closure does not match "
            "the declared reducer module bytes"
        )
    if document["tool_closure"] != expected_tool:
        refuse(
            f"{registration} is format 5, but tool_closure does not match "
            "the declared tool module bytes and bindings"
        )
    print(
        "ProjectRegistration migration check OK: "
        f"{registration.relative_to(root).as_posix()} is format {FORMAT_5} "
        "and both closures match."
    )
    return 0


def main() -> int:
    arguments = parse_arguments()
    try:
        return run(arguments)
    except (MigrationError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
