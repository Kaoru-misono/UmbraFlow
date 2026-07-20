#!/usr/bin/env python3
"""Replace the template project identity without touching shared infrastructure."""

from __future__ import annotations

import argparse
import re
from pathlib import Path


TEMPLATE_DESCRIPTION = "A reusable C++23 project template"
TEMPLATE_ONLY_BLOCK = re.compile(
    r"<!-- CPP_TEMPLATE_ONLY_BEGIN -->.*?<!-- CPP_TEMPLATE_ONLY_END -->\n?",
    re.DOTALL,
)
EXCLUDED_DIRECTORIES = {
    ".Codex",
    ".cache",
    ".git",
    ".worktrees",
    "build",
    "external",
    "install",
}


def split_words(value: str) -> list[str]:
    separated = re.sub(r"[_-]+", " ", value)
    separated = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", separated)
    separated = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1 \2", separated)
    return [word for word in separated.split() if word]


def validate_identifier(value: str, label: str) -> None:
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", value):
        raise ValueError(f"{label} must be an identifier: {value!r}")


def is_excluded(path: Path, root: Path) -> bool:
    relative = path.relative_to(root)
    return any(part in EXCLUDED_DIRECTORIES for part in relative.parts)


def replace_file(path: Path, replacements: dict[str, str], dry_run: bool) -> bool:
    try:
        content = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        return False

    updated = TEMPLATE_ONLY_BLOCK.sub("", content)
    for old, new in replacements.items():
        updated = updated.replace(old, new)

    if updated == content:
        return False

    if not dry_run:
        path.write_bytes(updated.encode("utf-8"))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Initialize a repository created from the C++ template."
    )
    parser.add_argument("name", help="CMake project name in PascalCase, such as MyProject")
    parser.add_argument("--description", default="A C++23 project")
    parser.add_argument("--slug", help="Executable/repository name; defaults to kebab-case")
    parser.add_argument("--namespace", dest="namespace_name", help="C++ root namespace")
    parser.add_argument("--macro-prefix", help="Uppercase macro prefix")
    parser.add_argument("--dry-run", action="store_true", help="list changed files without writing")
    args = parser.parse_args()

    try:
        validate_identifier(args.name, "project name")
    except ValueError as error:
        parser.error(str(error))
    words = split_words(args.name)
    if not words:
        parser.error("project name must contain at least one word")

    slug = args.slug or "-".join(word.lower() for word in words)
    namespace_name = args.namespace_name or "_".join(word.lower() for word in words)
    macro_prefix = args.macro_prefix or "_".join(word.upper() for word in words)

    if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", slug):
        parser.error(f"slug must use lowercase kebab-case: {slug!r}")
    try:
        validate_identifier(namespace_name, "namespace")
    except ValueError as error:
        parser.error(str(error))
    if not re.fullmatch(r"[A-Z][A-Z0-9_]*", macro_prefix):
        parser.error(f"macro prefix must use UPPER_CASE: {macro_prefix!r}")

    script_path = Path(__file__).resolve()
    root = script_path.parent.parent
    cmake_file = root / "CMakeLists.txt"
    if "project(CppTemplate" not in cmake_file.read_text(encoding="utf-8"):
        parser.error("template identity was not found; this repository may already be initialized")

    replacements = {
        TEMPLATE_DESCRIPTION: args.description,
        (
            "A reusable C++23 repository template derived from April2's build, coding, test,\n"
            "documentation, and agent workflows."
        ): args.description,
        (
            "The template root namespace is `cpp_template`; replace it during project "
            "initialization."
        ): f"The project root namespace is `{namespace_name}`.",
        "The template capability kernel currently provides:": (
            "The core capability kernel currently provides:"
        ),
        "The template uses April2's manifest-driven CMake module loader.": (
            "The project uses April2's manifest-driven CMake module loader."
        ),
        "## Template core additions": "## Core additions",
        "template, which ones should use": "project core, which ones should use",
        "template moves beyond C++23": "project moves beyond C++23",
        "template now would create": "project now would create",
        "general template core": "shared core",
        "general template": "shared core",
        "template has no borrow checker": "project has no borrow checker",
        "CppTemplate": args.name,
        "cpp-template": slug,
        "cpp_template": namespace_name,
        "CPP_TEMPLATE": macro_prefix,
    }

    changed_files = []
    for path in sorted(root.rglob("*")):
        if not path.is_file() or path.resolve() == script_path or is_excluded(path, root):
            continue
        if replace_file(path, replacements, args.dry_run):
            changed_files.append(path.relative_to(root))

    action = "Would update" if args.dry_run else "Updated"
    for path in changed_files:
        print(f"{action}: {path.as_posix()}")

    print()
    print(f"Project: {args.name}")
    print(f"Executable slug: {slug}")
    print(f"Namespace: {namespace_name}")
    print(f"Macro prefix: {macro_prefix}")
    print(f"Files: {len(changed_files)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
