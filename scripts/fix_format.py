#!/usr/bin/env python3
"""Normalize first-party repository text deterministically."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable
from pathlib import Path


CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
TEXT_EXTENSIONS = {
    *CPP_EXTENSIONS,
    ".bat",
    ".cfg",
    ".cmake",
    ".ini",
    ".json",
    ".manifest",
    ".md",
    ".ps1",
    ".py",
    ".rc",
    ".sh",
    ".toml",
    ".txt",
    ".xml",
    ".yaml",
    ".yml",
}
TEXT_FILENAMES = {
    ".clang-tidy",
    ".clangd",
    ".gitattributes",
    ".gitignore",
    "CMakeLists.txt",
}
EXCLUDED_DIRECTORY_NAMES = {
    ".cache",
    ".codex",
    ".git",
    ".reference",
    ".idea",
    ".worktrees",
    ".vscode",
    "__pycache__",
    "build",
    "external",
    "install",
}
EXCLUDED_PREFIXES = {
    (".claude", "worktrees"),
    ("tests", "external"),
}


def normalize(content: str, *, replace_tabs: bool) -> str:
    normalized = content.replace("\r\n", "\n").replace("\r", "\n")
    lines = normalized.split("\n")

    while lines and lines[-1] == "":
        lines.pop()

    normalized_lines = []
    for line in lines:
        line = line.rstrip(" \t")
        if replace_tabs:
            line = line.replace("\t", "    ")
        normalized_lines.append(line)

    if not normalized_lines:
        return ""

    return "\n".join(normalized_lines) + "\n"


def is_supported_text(path: Path) -> bool:
    return path.name in TEXT_FILENAMES or path.suffix.lower() in TEXT_EXTENSIONS


def is_excluded(path: Path, root: Path) -> bool:
    relative_parts = tuple(part.lower() for part in path.relative_to(root).parts)
    if any(part in EXCLUDED_DIRECTORY_NAMES for part in relative_parts[:-1]):
        return True

    return any(relative_parts[: len(prefix)] == prefix for prefix in EXCLUDED_PREFIXES)


def repository_files(root: Path) -> Iterable[Path]:
    for path in sorted(root.rglob("*")):
        if (
            path.is_file()
            and not path.is_symlink()
            and is_supported_text(path)
            and not is_excluded(path, root)
        ):
            yield path


def requested_files(root: Path, inputs: list[str]) -> Iterable[Path]:
    selected: set[Path] = set()

    for value in inputs:
        candidate = (root / value).resolve()
        try:
            candidate.relative_to(root)
        except ValueError as error:
            raise ValueError(f"path is outside the repository: {value}") from error

        if not candidate.exists():
            raise ValueError(f"path does not exist: {value}")

        if candidate.is_file():
            selected.add(candidate)
            continue

        for path in candidate.rglob("*"):
            if (
                path.is_file()
                and not path.is_symlink()
                and is_supported_text(path)
                and not is_excluded(path, root)
            ):
                selected.add(path)

    yield from sorted(selected)


def process(path: Path, *, check: bool) -> bool:
    original_bytes = path.read_bytes()
    if b"\0" in original_bytes:
        raise ValueError("contains a NUL byte")

    original = original_bytes.decode("utf-8")
    fixed = normalize(original, replace_tabs=path.suffix.lower() in CPP_EXTENSIONS)
    if fixed == original:
        return False

    if not check:
        path.write_bytes(fixed.encode("utf-8"))
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "paths",
        nargs="*",
        help="repository-relative files or directories; defaults to all first-party text",
    )
    parser.add_argument("--check", action="store_true", help="report changes without writing")
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    if not root.is_dir():
        parser.error(f"repository root does not exist: {root}")

    try:
        files = (
            list(requested_files(root, arguments.paths))
            if arguments.paths
            else list(repository_files(root))
        )
    except ValueError as error:
        parser.error(str(error))

    changed: list[Path] = []
    errors: list[tuple[Path, Exception]] = []
    for path in files:
        try:
            if process(path, check=arguments.check):
                changed.append(path.relative_to(root))
        except (OSError, UnicodeDecodeError, ValueError) as error:
            errors.append((path.relative_to(root), error))

    if changed:
        action = "Would normalize" if arguments.check else "Normalized"
        for path in changed:
            print(f"{action}: {path.as_posix()}")

    if errors:
        print("Text normalization errors:", file=sys.stderr)
        for path, error in errors:
            print(f"  {path.as_posix()}: {error}", file=sys.stderr)

    if arguments.check and changed:
        print(
            "Text normalization violations found; run python scripts/fix_format.py.",
            file=sys.stderr,
        )

    if errors or (arguments.check and changed):
        return 1

    print(f"Text normalization OK ({len(files)} files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
