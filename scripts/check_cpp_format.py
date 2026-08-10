#!/usr/bin/env python3
"""Check the project's mechanical C++ alignment rules.

This is deliberately a conservative recognizer, not a C++ formatter.  It checks
only adjacent, single-line member declarations and assignments whose structure
is unambiguous.  In particular, it skips pointer/reference declarators,
bitfields, function declarations, macros, preprocessor lines, comments, raw
strings, lambdas, operator declarations, templates containing commas,
assignments containing another ``=``, and every wrapped statement.  Local
classes are also skipped because reliably distinguishing their bodies requires
parsing arbitrary function and macro syntax.
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


SOURCE_EXTENSIONS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"}
SOURCE_ROOTS = ("modules", "entry", "tests", "contract-suite")
VENDORED_DIRECTORY_NAMES = {"external", "third_party"}

CLASS_DECLARATION = re.compile(
    r"^(?:class|struct) +[A-Za-z_][A-Za-z0-9_]*"
    r"(?: +final)?(?: *:[^;{]+)? *\{? *$"
)
NAMESPACE_DECLARATION = re.compile(
    r"^(?:inline +)?namespace"
    r"(?: +[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z_][A-Za-z0-9_]*)*)? *\{? *$"
)
# Member identifiers are matched by shape, not by the ``m_`` prefix: public
# ``struct`` members carry no prefix while private ``class`` members keep it, and
# both are data members the alignment rule governs.  What keeps this from
# swallowing arbitrary locals is the surrounding context, not the name: a member
# candidate is only accepted when the enclosing scope is a class or struct body
# (``eligible_class_body``), whereas locals live in ``other`` function-body
# scopes.  The keyword guard below removes the remaining non-member declarations
# that can also sit directly in a class body once the prefix no longer filters
# them out (friend declarations, nested forward declarations, typedefs).
MEMBER_DECLARATION = re.compile(
    r"^(?P<indent> *)"
    r"(?P<declarator>\S(?:.*\S)?)"
    r"(?P<gap> +)"
    r"(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"(?P<tail> *(?:\{[^{}]*\})? *; *)$"
)
NON_MEMBER_DECLARATOR_KEYWORDS = frozenset(
    {"friend", "typedef", "using", "template", "class", "struct", "union", "enum", "namespace"}
)
DESIGNATED_LEFT_HAND_SIDE = re.compile(r"\.[A-Za-z_][A-Za-z0-9_]*")
AUTO_INITIALIZER_LEFT_HAND_SIDE = re.compile(r"auto +[A-Za-z_][A-Za-z0-9_]*")
ASSIGNMENT_LEFT_HAND_SIDE = re.compile(
    r"[A-Za-z_][A-Za-z0-9_]*"
    r"(?:(?:::|\.)[A-Za-z_][A-Za-z0-9_]*)*"
)
MACRO_TOKEN = re.compile(r"\b[A-Z][A-Z0-9_]*_[A-Z0-9_]+\b")
MACRO_CALL = re.compile(r"\b[A-Z][A-Z0-9_]{2,} *\(")


@dataclass
class LexState:
    in_block_comment: bool = False
    raw_terminator: str | None = None
    in_preprocessor: bool = False


@dataclass(frozen=True)
class Candidate:
    line_index: int
    indent: str
    left_end: int
    target_start: int
    group_kind: str
    target_name: str


@dataclass(frozen=True)
class Violation:
    line_index: int
    target_name: str
    expected_column: int


def is_vendored(path: Path) -> bool:
    return any(part.lower() in VENDORED_DIRECTORY_NAMES for part in path.parts)


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


def mask_non_code(line: str, state: LexState) -> tuple[str, bool, bool]:
    """Replace comments and literals with spaces while preserving columns."""
    masked = list(line)
    has_comment = state.in_block_comment
    has_raw_string = state.raw_terminator is not None
    index = 0

    while index < len(line):
        if state.raw_terminator is not None:
            end = line.find(state.raw_terminator, index)
            if end < 0:
                masked[index:] = " " * (len(line) - index)
                break
            end += len(state.raw_terminator)
            masked[index:end] = " " * (end - index)
            index = end
            state.raw_terminator = None
            continue

        if state.in_block_comment:
            end = line.find("*/", index)
            if end < 0:
                masked[index:] = " " * (len(line) - index)
                break
            end += 2
            masked[index:end] = " " * (end - index)
            index = end
            state.in_block_comment = False
            continue

        if line.startswith("//", index):
            has_comment = True
            masked[index:] = " " * (len(line) - index)
            break

        if line.startswith("/*", index):
            has_comment = True
            state.in_block_comment = True
            masked[index : index + 2] = "  "
            index += 2
            continue

        raw_start = line.find('R"', index, index + 4)
        if raw_start == index or (
            raw_start > index
            and line[index:raw_start] in {"u8", "u", "U", "L"}
        ):
            prefix_start = index
            quote = line.find('"', prefix_start, prefix_start + 5)
            delimiter_end = line.find("(", quote + 1, quote + 19) if quote >= 0 else -1
            if delimiter_end >= 0:
                delimiter = line[quote + 1 : delimiter_end]
                if all(character not in " ()\\\t\r\n" for character in delimiter):
                    has_raw_string = True
                    state.raw_terminator = ")" + delimiter + '"'
                    end = line.find(state.raw_terminator, delimiter_end + 1)
                    if end < 0:
                        masked[prefix_start:] = " " * (len(line) - prefix_start)
                        break
                    end += len(state.raw_terminator)
                    masked[prefix_start:end] = " " * (end - prefix_start)
                    index = end
                    state.raw_terminator = None
                    continue

        if line[index] in {'"', "'"}:
            quote = line[index]
            start = index
            index += 1
            while index < len(line):
                if line[index] == "\\":
                    index += 2
                    continue
                index += 1
                if line[index - 1] == quote:
                    break
            masked[start:index] = " " * (index - start)
            continue

        index += 1

    return "".join(masked), has_comment, has_raw_string


def has_template_comma(code: str) -> bool:
    angle_depth = 0
    for character in code:
        if character == "<":
            angle_depth += 1
        elif character == ">" and angle_depth:
            angle_depth -= 1
        elif character == "," and angle_depth:
            return True
    return False


def has_macro(code: str) -> bool:
    return MACRO_TOKEN.search(code) is not None or MACRO_CALL.search(code) is not None


def member_candidate(
    line: str,
    code: str,
    line_index: int,
    *,
    eligible_class_body: bool,
    unsafe_line: bool,
) -> Candidate | None:
    if not eligible_class_body or unsafe_line or "\t" in line:
        return None
    if any(token in code for token in ("=", "*", "&", ",", "(", ")", "[", "]", ": ")):
        return None
    if has_template_comma(code) or has_macro(code):
        return None

    match = MEMBER_DECLARATION.fullmatch(line)
    if match is None:
        return None

    declarator = match.group("declarator")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_:<>]*(?: +[A-Za-z_][A-Za-z0-9_:<>]*)*", declarator):
        return None

    # A real data member's type is a type name.  A non-member declaration that
    # can also appear directly in a class body (friend, forward declaration,
    # typedef) is led by one of these keywords and is not aligned as a member.
    if any(token in NON_MEMBER_DECLARATOR_KEYWORDS for token in declarator.split()):
        return None

    return Candidate(
        line_index=line_index,
        indent=match.group("indent"),
        left_end=match.end("declarator"),
        target_start=match.start("name"),
        group_kind="member",
        target_name="member identifier",
    )


def assignment_candidate(
    line: str,
    code: str,
    line_index: int,
    *,
    direct_class_body: bool,
    unsafe_line: bool,
) -> Candidate | None:
    if direct_class_body or unsafe_line or "\t" in line:
        return None
    if "operator=" in code or any(token in code for token in ("[", "]", "->")):
        return None
    if has_template_comma(code) or has_macro(code) or code.count("=") != 1:
        return None

    operator_index = code.find("=")
    before = code[operator_index - 1] if operator_index else ""
    after = code[operator_index + 1] if operator_index + 1 < len(code) else ""
    if before in "=!<>+-*/%&|^" or after == "=":
        return None

    stripped_code = code.rstrip()
    if not stripped_code or stripped_code[-1] not in ";,":
        return None

    left_with_indent = code[:operator_index].rstrip()
    left = left_with_indent.lstrip(" ")
    indent_length = len(left_with_indent) - len(left)
    if not left or line[:indent_length] != " " * indent_length:
        return None
    if any(token in left for token in ("*", "&", "(", ")", "{", "}", ",")):
        return None

    designated = DESIGNATED_LEFT_HAND_SIDE.fullmatch(left) is not None
    auto_initializer = AUTO_INITIALIZER_LEFT_HAND_SIDE.fullmatch(left) is not None
    assignment = ASSIGNMENT_LEFT_HAND_SIDE.fullmatch(left) is not None
    if designated:
        if stripped_code[-1] != ",":
            return None
        group_kind = "designated assignment"
    elif auto_initializer or assignment:
        if stripped_code[-1] != ";":
            return None
        group_kind = "assignment"
    else:
        return None

    return Candidate(
        line_index=line_index,
        indent=" " * indent_length,
        left_end=len(left_with_indent),
        target_start=operator_index,
        group_kind=group_kind,
        target_name="assignment operator",
    )


def candidate_blocks(candidates: list[Candidate]) -> list[list[Candidate]]:
    blocks: list[list[Candidate]] = []
    block: list[Candidate] = []
    for candidate in candidates:
        if (
            block
            and (
                candidate.line_index != block[-1].line_index + 1
                or candidate.indent != block[-1].indent
                or candidate.group_kind != block[-1].group_kind
            )
        ):
            blocks.append(block)
            block = []
        block.append(candidate)
    if block:
        blocks.append(block)
    return blocks


def alignment_violations(candidates: list[Candidate]) -> list[Violation]:
    violations: list[Violation] = []
    for block in candidate_blocks(candidates):
        if len(block) < 2:
            continue
        longest_left = max(candidate.left_end for candidate in block)
        expected_column = longest_left + 1

        violations.extend(
            Violation(
                line_index=candidate.line_index,
                target_name=candidate.target_name,
                expected_column=expected_column,
            )
            for candidate in block
            if candidate.target_start != expected_column
        )
    return violations


def analyze_lines(lines: list[str]) -> tuple[list[Candidate], list[Violation]]:
    lex_state = LexState()
    scope_stack: list[str] = []
    pending_scope: str | None = None
    members: list[Candidate] = []
    assignments: list[Candidate] = []

    for line_index, line in enumerate(lines):
        masked, has_comment, has_raw_string = mask_non_code(line, lex_state)
        starts_preprocessor = masked.lstrip().startswith("#")
        preprocessor_line = lex_state.in_preprocessor or starts_preprocessor
        if preprocessor_line:
            lex_state.in_preprocessor = line.rstrip().endswith("\\")
            code = " " * len(masked)
        else:
            code = masked

        unsafe_line = has_comment or has_raw_string or preprocessor_line
        direct_class_body = bool(scope_stack) and scope_stack[-1] in {
            "class",
            "local class",
        }
        eligible_class_body = bool(scope_stack) and scope_stack[-1] == "class"

        member = member_candidate(
            line,
            code,
            line_index,
            eligible_class_body=eligible_class_body,
            unsafe_line=unsafe_line,
        )
        if member is not None:
            members.append(member)

        assignment = assignment_candidate(
            line,
            code,
            line_index,
            direct_class_body=direct_class_body,
            unsafe_line=unsafe_line,
        )
        if assignment is not None:
            assignments.append(assignment)

        stripped = code.strip()
        declaration_scope: str | None = None
        if CLASS_DECLARATION.fullmatch(stripped):
            parent_is_declarative = all(
                scope in {"namespace", "class"} for scope in scope_stack
            )
            declaration_scope = "class" if parent_is_declarative else "local class"
        elif NAMESPACE_DECLARATION.fullmatch(stripped):
            declaration_scope = "namespace"

        if declaration_scope is not None:
            pending_scope = declaration_scope

        first_open_uses_pending = pending_scope is not None
        for character in code:
            if character == "}":
                if scope_stack:
                    scope_stack.pop()
            elif character == "{":
                if first_open_uses_pending:
                    scope_stack.append(pending_scope or "other")
                    pending_scope = None
                    first_open_uses_pending = False
                else:
                    scope_stack.append("other")

        if pending_scope is not None and ";" in code:
            pending_scope = None

    candidates = members + assignments
    violations = alignment_violations(members) + alignment_violations(assignments)
    return candidates, violations


def split_preserving_endings(text: str) -> list[str]:
    return text.splitlines(keepends=True)


def line_body(line: str) -> tuple[str, str]:
    if line.endswith("\r\n"):
        return line[:-2], "\r\n"
    if line.endswith(("\n", "\r")):
        return line[:-1], line[-1]
    return line, ""


def check_file(path: Path, *, fix: bool) -> list[Violation]:
    original = path.read_bytes()
    text = original.decode("utf-8")
    physical_lines = split_preserving_endings(text)
    bodies = [line_body(line)[0] for line in physical_lines]
    candidates, violations = analyze_lines(bodies)

    if fix and violations:
        expected_by_line = {
            violation.line_index: violation.expected_column for violation in violations
        }
        candidate_by_line = {
            candidate.line_index: candidate
            for candidate in candidates
            if candidate.line_index in expected_by_line
        }
        for line_index, expected_column in expected_by_line.items():
            body, ending = line_body(physical_lines[line_index])
            candidate = candidate_by_line[line_index]
            gap = " " * (expected_column - candidate.left_end)
            body = (
                body[: candidate.left_end]
                + gap
                + body[candidate.target_start :]
            )
            physical_lines[line_index] = body + ending
        updated = "".join(physical_lines).encode("utf-8")
        if updated != original:
            path.write_bytes(updated)

    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--fix",
        action="store_true",
        help="apply recognized member and assignment alignment in place",
    )
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root containing modules, entry, or tests",
    )
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    if not root.is_dir():
        parser.error(f"repository root does not exist: {root}")

    files = source_files(root)
    violations: list[tuple[Path, Violation]] = []
    try:
        for path in files:
            violations.extend(
                (path, violation)
                for violation in check_file(path, fix=arguments.fix)
            )
    except (OSError, UnicodeError) as error:
        print(f"C++ alignment check failed: {error}", file=sys.stderr)
        return 2

    if not arguments.fix and violations:
        for path, violation in violations:
            relative_path = path.relative_to(root).as_posix()
            print(
                f"{relative_path}:{violation.line_index + 1}: "
                f"{violation.target_name} should be in column "
                f"{violation.expected_column + 1}",
                file=sys.stderr,
            )
        print(
            "C++ alignment violations found; run "
            "python scripts/check_cpp_format.py --fix.",
            file=sys.stderr,
        )
        return 1

    action = "C++ alignment applied" if arguments.fix else "C++ format check OK"
    print(f"{action} ({len(files)} files).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
