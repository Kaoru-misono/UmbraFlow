#!/usr/bin/env python3
"""Enforce the repository's safe C++ boundary rules."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SOURCE_EXTENSIONS = {".cpp", ".h", ".hpp"}
SOURCE_ROOTS = ("modules", "entry")
UNSAFE_DIRECTORY_NAMES = {"external", "ffi", "platform", "unsafe"}
VENDORED_DIRECTORY_NAMES = {"external", "third_party"}
SAFETY_COMMENT = "// SAFETY:"


@dataclass(frozen=True)
class Rule:
    name: str
    pattern: re.Pattern[str]
    boundary_allowed: bool


RULES = (
    Rule("reinterpret_cast", re.compile(r"\breinterpret_cast\s*<"), True),
    Rule("const_cast", re.compile(r"\bconst_cast\s*<"), True),
    Rule("raw allocation", re.compile(r"\b(?:new|delete)\b"), True),
    Rule(
        "C allocation",
        re.compile(r"\b(?:(?:std::)?(?:malloc|calloc|realloc|free))\s*\("),
        True,
    ),
    Rule("direct unreachable UB", re.compile(r"\bstd::unreachable\s*\("), False),
    Rule("detached thread", re.compile(r"\.detach\s*\("), False),
    Rule(
        "ADR-011 forbidden SetForegroundWindow use",
        re.compile(r"\bSetForegroundWindow\b"),
        False,
    ),
    Rule("ADR-011 forbidden SetFocus use", re.compile(r"\bSetFocus\b"), False),
    Rule("ADR-011 forbidden SendInput use", re.compile(r"\bSendInput\b"), False),
    Rule("ADR-011 forbidden mouse_event use", re.compile(r"\bmouse_event\b"), False),
    Rule("ADR-011 forbidden keybd_event use", re.compile(r"\bkeybd_event\b"), False),
    Rule("ADR-011 forbidden SetCursorPos use", re.compile(r"\bSetCursorPos\b"), False),
    Rule(
        "ADR-011 forbidden BringWindowToTop use",
        re.compile(r"\bBringWindowToTop\b"),
        False,
    ),
    Rule(
        "ADR-011 forbidden SwitchToThisWindow use",
        re.compile(r"\bSwitchToThisWindow\b"),
        False,
    ),
    Rule(
        "ADR-011 forbidden AttachThreadInput use",
        re.compile(r"\bAttachThreadInput\b"),
        False,
    ),
    Rule(
        "ADR-011 forbidden SetActiveWindow use",
        re.compile(r"\bSetActiveWindow\b"),
        False,
    ),
)

MUST_USE_FUNCTION = re.compile(
    r"(?P<nodiscard>\[\[nodiscard(?:\([^\]]*\))?\]\]\s*)?"
    r"(?P<specifiers>(?:(?:inline|static|constexpr|friend)\s+)*)"
    r"auto\s+(?P<name>[A-Za-z_~][A-Za-z0-9_:~]*)\s*"
    r"(?P<parameters>\([^;{}]*\))\s*"
    r"(?:const\s*)?"
    r"(?:noexcept(?:\s*\([^;{}]*\))?\s*)?"
    r"(?:UF_LIFETIME_BOUND\s*)?"
    r"->\s*"
    r"(?:[A-Za-z_][A-Za-z0-9_:]*::)?"
    r"(?:Result\s*<|Status\b|std::optional\s*<)",
    re.DOTALL,
)


def missing_must_use_nodiscard_lines(masked_content: str) -> list[int]:
    matches = list(MUST_USE_FUNCTION.finditer(masked_content))
    annotated_functions = {
        (
            match.group("name"),
            re.sub(r"\s+", " ", match.group("parameters")),
        )
        for match in matches
        if match.group("nodiscard") is not None
    }
    missing_lines: list[int] = []

    for match in matches:
        function_key = (
            match.group("name"),
            re.sub(r"\s+", " ", match.group("parameters")),
        )
        if match.group("nodiscard") is not None:
            continue

        specifiers = match.group("specifiers").split()
        if "friend" in specifiers and function_key in annotated_functions:
            continue

        missing_lines.append(masked_content.count("\n", 0, match.start()) + 1)

    return missing_lines


def strip_line_comment(line: str) -> str:
    comment = line.find("//")
    return line if comment < 0 else line[:comment]


def mask_deleted_special_members(line: str) -> str:
    return re.sub(r"=\s*delete\b", "", line)


def mask_non_code(content: str) -> str:
    patterns = (
        re.compile(r"/\*.*?\*/", re.DOTALL),
        re.compile(r'R"([^\s()\\]{0,16})\(.*?\)\1"', re.DOTALL),
        re.compile(r'"(?:\\.|[^"\\])*"'),
        re.compile(r"'(?:\\.|[^'\\])*'"),
    )

    masked = content
    for pattern in patterns:
        masked = pattern.sub(
            lambda match: "".join(
                "\n" if character == "\n" else " " for character in match.group(0)
            ),
            masked,
        )
    return masked


def is_unsafe_boundary(path: Path) -> bool:
    return any(part.lower() in UNSAFE_DIRECTORY_NAMES for part in path.parts)


def is_vendored(path: Path) -> bool:
    return any(part.lower() in VENDORED_DIRECTORY_NAMES for part in path.parts)


def has_safety_comment(lines: list[str], line_index: int) -> bool:
    first = max(0, line_index - 3)
    return any(SAFETY_COMMENT in line for line in lines[first:line_index])


def source_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for source_root in SOURCE_ROOTS:
        directory = root / source_root
        if not directory.is_dir():
            continue
        files.extend(
            path
            for path in directory.rglob("*")
            if (
                path.is_file()
                and path.suffix in SOURCE_EXTENSIONS
                and not is_vendored(path.relative_to(root))
            )
        )
    return sorted(files)


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    arguments = argument_parser.parse_args()

    root = arguments.root.resolve()
    violations: list[str] = []
    checked_files = source_files(root)

    for path in checked_files:
        relative = path.relative_to(root)
        content = path.read_text(encoding="utf-8")
        lines = content.splitlines()
        code_lines = mask_non_code(content).splitlines()
        boundary = is_unsafe_boundary(relative)

        for line_index, line in enumerate(code_lines):
            code = mask_deleted_special_members(strip_line_comment(line))
            for rule in RULES:
                if rule.pattern.search(code) is None:
                    continue

                line_number = line_index + 1
                if not rule.boundary_allowed:
                    violations.append(
                        f"{relative.as_posix()}:{line_number}: {rule.name} is forbidden"
                    )
                elif not boundary:
                    violations.append(
                        f"{relative.as_posix()}:{line_number}: {rule.name} must be isolated "
                        "under an unsafe, platform, or ffi directory"
                    )
                elif not has_safety_comment(lines, line_index):
                    violations.append(
                        f"{relative.as_posix()}:{line_number}: {rule.name} requires a nearby "
                        f"{SAFETY_COMMENT} justification"
                    )

        if "modules/core" in relative.as_posix():
            for line_index, line in enumerate(code_lines):
                if re.search(r"\bthrow\b", strip_line_comment(line)) is not None:
                    violations.append(
                        f"{relative.as_posix()}:{line_index + 1}: core must not throw explicitly"
                    )

        if path.suffix == ".hpp":
            masked_content = "\n".join(code_lines)
            for line_number in missing_must_use_nodiscard_lines(masked_content):
                violations.append(
                    f"{relative.as_posix()}:{line_number}: Result, Status, and optional "
                    "functions must be [[nodiscard]]"
                )

    if violations:
        print("Safe C++ boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print(f"Safe C++ boundary check OK ({len(checked_files)} files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
