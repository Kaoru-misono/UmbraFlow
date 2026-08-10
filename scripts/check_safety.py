#!/usr/bin/env python3
"""Enforce the repository's safe C++ boundary rules."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import member_init


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
# contract-suite is first-party C++ that ships to consumers, so it is held to
# the same boundary rules as everything under modules, entry and tests.
SOURCE_ROOTS = ("modules", "entry", "tests", "contract-suite")
# Exactly the three boundary directories the coding standard names. A vendored
# directory is never one of them: it is dropped from the scan below, so a
# vendored name here would advertise a boundary the gate can never reach.
UNSAFE_DIRECTORY_NAMES = {"ffi", "platform", "unsafe"}
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
    # Foreground activation, global input injection and real cursor movement
    # are absent from the capability surface, not a fallback: once a task
    # declares `background_only`, an incompatible target is an explicit failure
    # and never a silent degradation to one of these. Deciding artifact:
    # decision 1 of docs/plans/2026-07-21-product-form-and-roadmap.md.
    Rule(
        "background_only forbidden SetForegroundWindow use",
        re.compile(r"\bSetForegroundWindow\b"),
        False,
    ),
    Rule("background_only forbidden SetFocus use", re.compile(r"\bSetFocus\b"), False),
    Rule(
        "background_only forbidden SendInput use",
        re.compile(r"\bSendInput\b"),
        False,
    ),
    Rule(
        "background_only forbidden mouse_event use",
        re.compile(r"\bmouse_event\b"),
        False,
    ),
    Rule(
        "background_only forbidden keybd_event use",
        re.compile(r"\bkeybd_event\b"),
        False,
    ),
    Rule(
        "background_only forbidden SetCursorPos use",
        re.compile(r"\bSetCursorPos\b"),
        False,
    ),
    Rule(
        "background_only forbidden BringWindowToTop use",
        re.compile(r"\bBringWindowToTop\b"),
        False,
    ),
    Rule(
        "background_only forbidden SwitchToThisWindow use",
        re.compile(r"\bSwitchToThisWindow\b"),
        False,
    ),
    Rule(
        "background_only forbidden AttachThreadInput use",
        re.compile(r"\bAttachThreadInput\b"),
        False,
    ),
    Rule(
        "background_only forbidden SetActiveWindow use",
        re.compile(r"\bSetActiveWindow\b"),
        False,
    ),
)

MUST_USE_FUNCTION = re.compile(
    r"(?P<nodiscard>\[\[nodiscard(?:\([^\]]*\))?\]\]\s*)?"
    r"(?P<specifiers>(?:(?:inline|static|constexpr|friend|virtual)\s+)*)"
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


# One alternation, not four passes, because these constructs contain each
# other's delimiters and only a single left-to-right scan resolves that the way
# a lexer does: whichever starts first consumes its own span.
#
# Four separate passes had a hole that could HIDE a violation rather than
# invent one. Line comments were not masked at all, so an apostrophe in a
# comment -- "the caller's buffer" -- opened a character literal that ran to
# the next apostrophe, blanking every line between. A raw `new` sitting in
# that gap was invisible to the rules, and the gate passed the file.
#
# Line comments are masked here; `has_safety_comment` reads the ORIGINAL
# lines, so `// SAFETY:` justifications are still found.
#
# The digit separator is the same hole from the other side, and it HID
# violations for as long as the one above did: `1'000` is not a character
# literal, but the character-literal branch reads its apostrophe as an opening
# quote and blanks everything up to the next apostrophe in the file -- which,
# in a test full of `R"lua(` blocks, was thousands of lines including the raw
# strings' own openers. It is matched first, before the branch that would
# misread it, and blanked like every other non-code byte because a separator
# carries no meaning any rule below asks about.
#
# Its lookbehind must keep excluding the `u8` encoding prefix, whose `8` is a
# hex digit: unguarded, the branch eats the opening quote of `u8'a'`. Excluding
# a raw newline from the character-literal class caps any apostrophe that still
# escapes at one line instead of the rest of the file.
NON_CODE_PATTERN = re.compile(
    r'R"([^\s()\\]{0,16})\(.*?\)\1"'  # raw string, may contain anything
    r"|/\*.*?\*/"  # block comment
    r"|//[^\n]*"  # line comment
    r'|"(?:\\.|[^"\\])*"'  # string literal
    r"|(?<![uU]8)(?<=[0-9a-fA-F])'(?=[0-9a-fA-F])"  # digit separator, not a literal
    r"|'(?:\\.|[^'\\\n])*'",  # character literal, never spans a line
    re.DOTALL,
)


def mask_non_code(content: str) -> str:
    return NON_CODE_PATTERN.sub(
        lambda match: "".join(
            "\n" if character == "\n" else " " for character in match.group(0)
        ),
        content,
    )


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
                and path.suffix.lower() in SOURCE_EXTENSIONS
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
    member_sources: list[tuple[str, str]] = []

    for path in checked_files:
        relative = path.relative_to(root)
        content = path.read_text(encoding="utf-8")
        member_sources.append((relative.as_posix(), content))
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

        if path.suffix.lower() == ".hpp":
            masked_content = "\n".join(code_lines)
            for line_number in missing_must_use_nodiscard_lines(masked_content):
                violations.append(
                    f"{relative.as_posix()}:{line_number}: Result, Status, and optional "
                    "functions must be [[nodiscard]]"
                )

    violations.extend(member_init.violations(member_sources))

    if violations:
        print("Safe C++ boundary violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        return 1

    print(f"Safe C++ boundary check OK ({len(checked_files)} files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
