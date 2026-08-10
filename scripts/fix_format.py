#!/usr/bin/env python3
"""Normalize first-party repository text deterministically."""

from __future__ import annotations

import argparse
import sys
from collections.abc import Iterable
from pathlib import Path


CPP_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}

# Indented with spaces by convention, so a tab is a defect rather than a choice.
# The trusted Luau framework is source the build embeds into the binary, and
# until it was listed here nothing in the repository checked it at all.
SPACE_INDENTED_EXTENSIONS = {*CPP_EXTENSIONS, ".luau"}
TEXT_EXTENSIONS = {
    *SPACE_INDENTED_EXTENSIONS,
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

# The encoding prefixes a raw string literal may carry ahead of its R. Longest
# first, so u8R"..." is not read as u followed by 8R.
RAW_STRING_PREFIXES = ("u8", "u", "U", "L")


def is_identifier_character(value: str) -> bool:
    return value.isalnum() or value == "_"


def skip_line_comment(content: str, index: int) -> int:
    """The index of the newline that ends the // comment starting at index."""
    while True:
        newline = content.find("\n", index)
        if newline < 0:
            return len(content)
        if newline == 0 or content[newline - 1] != "\\":
            return newline
        index = newline + 1


def raw_string_at(content: str, quote: int) -> tuple[int, str] | None:
    """The body start and closing token of the raw string opening at quote."""
    if quote == 0 or content[quote - 1] != "R":
        return None

    start = quote - 1
    for prefix in RAW_STRING_PREFIXES:
        if start >= len(prefix) and content[start - len(prefix) : start] == prefix:
            start -= len(prefix)
            break
    if start > 0 and is_identifier_character(content[start - 1]):
        return None

    body = content.find("(", quote + 1)
    if body < 0:
        return None

    # A d-char-sequence holds at most 16 characters and no whitespace, so a
    # parenthesis further down the file does not open a raw string.
    delimiter = content[quote + 1 : body]
    if len(delimiter) > 16 or any(character.isspace() for character in delimiter):
        return None

    return body + 1, ")" + delimiter + '"'


def mark_quoted(content: str, quote: int, mask: list[bool]) -> int:
    """Mark an ordinary string or character literal and return the index past it."""
    terminator = content[quote]
    index = quote + 1
    while index < len(content):
        character = content[index]
        if character == "\\" and index + 1 < len(content):
            mask[index] = True
            mask[index + 1] = True
            index += 2
            continue
        if character == terminator:
            return index + 1
        if character == "\n":
            return index
        mask[index] = True
        index += 1
    return len(content)


def mark_literals(content: str) -> list[bool]:
    """Mark every character that carries a literal's value rather than layout.

    Whitespace inside a literal is data. A raw string carries it across line
    boundaries, where stripping a trailing space or widening a tab rewrites the
    program's meaning while it still compiles: modules/operator's ledger pins a
    fingerprint over the exact text of an R"sql(...)" block, and a format pass
    that edited those bytes would make the store refuse every database already
    written, with recomputing the fingerprint -- the documented remedy -- the
    step that made the break permanent.
    """
    mask = [False] * len(content)
    size = len(content)
    index = 0
    while index < size:
        if content.startswith("//", index):
            index = skip_line_comment(content, index)
            continue
        if content.startswith("/*", index):
            end = content.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue

        character = content[index]
        if character == '"':
            raw = raw_string_at(content, index)
            if raw is None:
                index = mark_quoted(content, index, mask)
                continue
            body, terminator = raw
            end = content.find(terminator, body)
            for position in range(body, size if end < 0 else end):
                mask[position] = True
            index = size if end < 0 else end + len(terminator)
            continue

        # A digit separator is not a character literal: 1'000 must not open one.
        if character == "'" and not (
            index > 0 and is_identifier_character(content[index - 1])
        ):
            index = mark_quoted(content, index, mask)
            continue

        index += 1
    return mask


def line_masks(mask: list[bool], lines: list[str]) -> list[list[bool]]:
    masks = []
    offset = 0
    for line in lines:
        masks.append(mask[offset : offset + len(line)])
        offset += len(line) + 1
    return masks


def normalize(content: str, *, replace_tabs: bool, protect_literals: bool) -> str:
    normalized = content.replace("\r\n", "\n").replace("\r", "\n")
    lines = normalized.split("\n")
    masks = (
        line_masks(mark_literals(normalized), lines)
        if protect_literals
        else [[False] * len(line) for line in lines]
    )

    while lines and lines[-1] == "":
        lines.pop()
        masks.pop()

    normalized_lines = []
    for line, mask in zip(lines, masks):
        end = len(line)
        while end > 0 and line[end - 1] in " \t" and not mask[end - 1]:
            end -= 1
        kept = line[:end]
        if replace_tabs:
            kept = "".join(
                "    " if character == "\t" and not protected else character
                for character, protected in zip(kept, mask[:end])
            )
        normalized_lines.append(kept)

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
    fixed = normalize(
        original,
        replace_tabs=path.suffix.lower() in SPACE_INDENTED_EXTENSIONS,
        protect_literals=path.suffix.lower() in CPP_EXTENSIONS,
    )
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
