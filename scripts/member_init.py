#!/usr/bin/env python3
"""Analyze data-member initialization against the project coding standard.

The standard requires that every stored data member carry an in-class brace
initializer unless the value must come from construction, and that no member
carry both an in-class initializer and a member-initializer-list entry in every
constructor. This module reports both failures:

`dead`     a member with an in-class initializer that every constructor also
           names, so the constructor always wins and the in-class initializer
           is dead code advertising a valid default state that never applies.
`missing`  a member with no in-class initializer whose type is unconditionally
           default-constructible, reachable by a construction path that leaves
           it default-initialized.

This is a deliberately conservative recognizer, not a C++ parser. It gates CI,
so a false positive is far more expensive than a miss. Every judgment therefore
requires positive evidence: when a class cannot be parsed with confidence the
analyzer stays silent rather than guessing. In particular, an empty set of
discovered constructors never means "nothing initializes this member" — it
means the constructors were not found, which is a reason to say nothing.

Recognized and deliberately skipped: class templates and explicit
specializations, class heads carrying an export or attribute macro, any class
name declared by more than one body, out-of-line constructors spelled through a
nested, qualified, or template-qualified name, member-initializer lists broken
by a preprocessor directive, and classes that inherit constructors. Whether an
arbitrary member *could* be brace-initialized is a question only the compiler
can answer, so `missing` is further restricted to an allowlist of types whose
default constructor is guaranteed.

`cppcoreguidelines-pro-type-member-init` in the `clang-analysis` CI job is meant
to cover the indeterminate cases this misses. It can only do so once two
properties hold: that job compiles, and `.clang-tidy`'s `HeaderFilterRegex`
matches first-party headers — class definitions here live in headers, so the
check reports at the constructor in the `.hpp`. As of 2026-08-10 neither held,
and the check had caught nothing to date; see
`docs/pitfalls/checks-that-cannot-fail.md`. Until both hold, every member this
recognizer stays silent on is reader-enforced.
"""

from __future__ import annotations

import re
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path


CLASS_HEAD = re.compile(
    r"\b(?:class|struct)\s+(?P<head>[\w\s]+?)\s*(?:final\b\s*)?(?::[^;{]*)?\{"
)

MEMBER_DECLARATION = re.compile(
    r"^\s*(?P<type>(?:mutable\s+)?[\w:][\w:<>,\s\*&]*?)\s"
    r"(?P<name>m_\w+)\s*(?P<initializer>\{[^;]*\}|=\s*[^;]+)?\s*;\s*$"
)

# Types whose default constructor is guaranteed by the standard, so `{}` always
# compiles. std::array is excluded on purpose: it default-constructs only when
# its element type does.
ALWAYS_DEFAULT_CONSTRUCTIBLE = re.compile(
    r"^(?:mutable\s+)?(?:"
    r"bool|char|(?:un)?signed\s+\w+|float|double|std::byte|std::size_t"
    r"|u?int(?:8|16|32|64)|uintptr|intptr|uintmax|intmax"
    r"|std::string|std::string_view|std::filesystem::path"
    r"|std::vector<.*>|std::deque<.*>|std::(?:unordered_)?map<.*>"
    r"|std::(?:unordered_)?set<.*>|std::optional<.*>"
    r"|std::shared_ptr<.*>|std::unique_ptr<.*>|std::weak_ptr<.*>"
    r"|std::atomic<.*>|std::mutex|std::function<.*>"
    r")\s*$"
)

INITIALIZER_ITEM = re.compile(r"\s*([\w:]+(?:\s*<[^;{]*?>)?)\s*([\{(])")
LIST_SEPARATOR = re.compile(r"\s*,")
DECLARED_DELETED = re.compile(r"^[^;{]*=\s*delete")
DECLARED_DEFAULTED = re.compile(r"^[^;{]*=\s*default")
INHERITED_CONSTRUCTOR = re.compile(r"\busing\s+[\w:]*(\w+)\s*::\s*\1\s*;")

# A member-initializer list that exists but could not be read with confidence.
UNREADABLE = "unreadable"


@dataclass
class Member:
    name: str
    type_name: str
    line: int
    has_initializer: bool


@dataclass
class ClassRecord:
    """Everything known about one class name across the whole repository."""

    members: list[Member] = field(default_factory=list)
    member_file: str = ""
    constructors: list[set[str]] = field(default_factory=list)
    declared: int = 0
    bodies: int = 0
    inherits_constructors: bool = False
    unreadable: bool = False


@dataclass(frozen=True)
class Finding:
    file: str
    line: int
    class_name: str
    member: str
    kind: str


def blank_like(text: str) -> str:
    """Spaces of the same length, with newlines kept so line numbers survive."""
    return "".join("\n" if character == "\n" else " " for character in text)


def normalize(text: str) -> str:
    """Blank out comments and literals while preserving every source offset.

    Removing a block comment outright would delete its newlines and shift every
    reported line number after it. Leaving literals in place would let a brace
    or parenthesis inside `"a)b"` unbalance the class body.
    """
    patterns = (
        re.compile(r"//[^\n]*"),
        re.compile(r"/\*.*?\*/", re.DOTALL),
        re.compile(r'R"([^\s()\\]{0,16})\(.*?\)\1"', re.DOTALL),
        re.compile(r'"(?:\\.|[^"\\\n])*"'),
        re.compile(r"'(?:\\.|[^'\\\n])*'"),
    )
    for pattern in patterns:
        text = pattern.sub(lambda match: blank_like(match.group(0)), text)
    return text


def blank_template_parameters(text: str) -> str:
    """Blank out `template <...>` parameter lists.

    `template <class Element>` otherwise reads as a class head and invents a
    class named `Element` holding the real class's members.
    """
    result = list(text)
    for match in re.finditer(r"\btemplate\s*<", text):
        depth = 0
        for index in range(match.end() - 1, len(text)):
            if text[index] == "<":
                depth += 1
            elif text[index] == ">":
                depth -= 1
                if depth == 0:
                    for position in range(match.start(), index + 1):
                        if result[position] != "\n":
                            result[position] = " "
                    break
    return "".join(result)


def matching_brace(text: str, start: int) -> int | None:
    """Index of the brace or parenthesis closing the one at `start`."""
    depth = 0
    for index in range(start, len(text)):
        if text[index] in "{(":
            depth += 1
        elif text[index] in "})":
            depth -= 1
            if depth == 0:
                return index
    return None


def class_name_of(head: str) -> str | None:
    """The declared name from a class head, or None if it is not a plain name.

    `class UF_CORE_API Frame final` must yield `Frame`, not the export macro.
    """
    words = [word for word in head.split() if word != "final"]
    return words[-1] if words else None


def class_bodies(text: str):
    """Yield (name, body, body_offset) for every class and struct definition."""
    for match in CLASS_HEAD.finditer(text):
        name = class_name_of(match.group("head"))
        if name is None:
            continue
        opening = match.end() - 1
        closing = matching_brace(text, opening)
        if closing is not None:
            yield name, text[opening + 1 : closing], opening + 1


def top_level_lines(body: str):
    """Yield (offset, line) for body lines at brace depth zero.

    Nested classes and function bodies sit at a deeper level, so their contents
    never appear as members of the enclosing class.
    """
    depth = 0
    current: list[str] = []
    start = 0
    for index, character in enumerate(body):
        if character == "\n":
            if depth == 0:
                yield start, "".join(current)
            current = []
            start = index + 1
            continue
        if character in "{(":
            depth += 1
        elif character in "})":
            depth -= 1
        current.append(character)


def depth_at(text: str) -> list[int]:
    """Brace and parenthesis depth immediately before each offset."""
    depths = [0] * (len(text) + 1)
    depth = 0
    for index, character in enumerate(text):
        depths[index] = depth
        if character in "{(":
            depth += 1
        elif character in "})":
            depth -= 1
    depths[len(text)] = depth
    return depths


def is_copy_or_move(parameters: str, class_name: str) -> bool:
    """Whether a parameter list is that of a copy or move constructor.

    Such a constructor initializes every member from the source object. It
    neither leaves a member default-initialized nor keeps an in-class
    initializer live, so it must not count as a construction path. Both const
    spellings appear in practice and both must be recognized.
    """
    first = parameters.split(",")[0].strip()
    return (
        re.fullmatch(
            rf"(?:const\s+)?{class_name}\s*(?:const\s*)?&&?\s*\w*", first
        )
        is not None
    )


def initializer_list_names(text: str) -> set[str] | None | str:
    """Names in the member-initializer list following a constructor signature.

    Returns None when the constructor has no list, the sentinel UNREADABLE when
    a list exists but cannot be read with confidence, and otherwise the set of
    members it initializes. The three states must stay distinct: treating an
    unreadable list as an absent one yields an empty set, which reads as "this
    constructor initializes nothing" and reports every member.

    The list is parsed as the comma-separated sequence of
    `name{...}` / `name(...)` items that it is; locating the constructor body by
    searching for '{' does not work, because `m_bytes{bytes}` contains braces
    and `m_value(std::move(value))` puts a ')' directly before the body brace.
    """
    start = None
    index = 0
    while index < len(text):
        if text[index : index + 2] == "::":
            index += 2
            continue
        if text[index] == ":":
            start = index + 1
            break
        if text[index] in "{;":
            return None
        index += 1
    if start is None:
        return None

    names: set[str] = set()
    index = start
    while True:
        item = INITIALIZER_ITEM.match(text, index)
        if item is None:
            return names
        names.add(item.group(1))
        closing = "}" if item.group(2) == "{" else ")"
        end = matching_brace(text, item.end() - 1)
        if end is None or text[end] != closing:
            return names
        separator = LIST_SEPARATOR.match(text, end + 1)
        if separator is None:
            # A directive inside the list hides the remaining entries, so the
            # set would be silently short.
            return UNREADABLE if "#" in text[end : end + 200] else names
        index = separator.end()


def constructor_paths(text: str, class_name: str, body_only: bool):
    """Yield (state, initialized_members) for each constructor found.

    State is "defined", "declared" for a declaration whose definition is
    elsewhere, or "unreadable" when the initializer list could not be parsed.

    An empty set means the constructor leaves every in-class initializer live.
    Destructors, deleted constructors, copy and move constructors, and
    delegating constructors are excluded: none of them is a path that can leave
    a member default-initialized.
    """
    if body_only:
        pattern = rf"(?<![\w:~]){class_name}\s*\("
        depths = depth_at(text)
    else:
        pattern = rf"(?<![\w:~]){class_name}::{class_name}\s*\("
        depths = None

    for match in re.finditer(pattern, text):
        if depths is not None and depths[match.start()] != 0:
            continue  # a construction inside a member function body, not a ctor
        opening = match.end() - 1
        closing = matching_brace(text, opening)
        if closing is None:
            continue
        if is_copy_or_move(text[opening + 1 : closing], class_name):
            continue
        tail = text[closing + 1 : closing + 4000]
        if DECLARED_DELETED.match(tail):
            continue

        names = initializer_list_names(tail)
        if names is UNREADABLE:
            yield "unreadable", set()
        elif names is not None:
            if class_name in names:
                continue  # delegating; the target constructor initializes all
            yield "defined", {name for name in names if name.startswith("m_")}
        elif DECLARED_DEFAULTED.match(tail) or "{" in tail.split(";")[0]:
            yield "defined", set()
        else:
            yield "declared", set()  # declaration only; defined out of line


def collect(files: list[tuple[str, str]]) -> dict[str, ClassRecord]:
    """Index every class across all files so out-of-line constructors are seen."""
    records: dict[str, ClassRecord] = defaultdict(ClassRecord)

    for relative, raw in files:
        text = blank_template_parameters(normalize(raw))
        line_of = [1] * (len(text) + 1)
        line = 1
        for index, character in enumerate(text):
            line_of[index] = line
            if character == "\n":
                line += 1
        line_of[len(text)] = line

        for name, body, offset in class_bodies(text):
            record = records[name]
            record.bodies += 1
            if INHERITED_CONSTRUCTOR.search(body):
                record.inherits_constructors = True

            found: list[Member] = []
            for position, source_line in top_level_lines(body):
                hit = MEMBER_DECLARATION.match(source_line)
                if hit is not None:
                    found.append(
                        Member(
                            name=hit.group("name"),
                            type_name=hit.group("type").strip(),
                            line=line_of[offset + position],
                            has_initializer=hit.group("initializer") is not None,
                        )
                    )
            if found and not record.members:
                record.members = found
                record.member_file = relative

            for state, names in constructor_paths(body, name, body_only=True):
                record.declared += 1
                if state == "unreadable":
                    record.unreadable = True
                elif state == "defined":
                    record.constructors.append(names)

        for name in {
            match.group(1)
            for match in re.finditer(r"(?<![\w:~])(\w+)::\1\s*\(", text)
        }:
            for _, names in constructor_paths(text, name, body_only=False):
                records[name].constructors.append(names)

    return records


def findings(records: dict[str, ClassRecord]) -> list[Finding]:
    results: list[Finding] = []

    for class_name, record in records.items():
        if not record.members or record.bodies != 1:
            continue  # a name declared more than once is not worth a report

        constructors = record.constructors
        # A class that declares constructors but whose definitions were not
        # found is unparsed, not constructor-free. Saying nothing is correct.
        unparsed = record.unreadable or (record.declared > 0 and not constructors)
        complete = (
            bool(constructors)
            and not record.unreadable
            and len(constructors) >= record.declared
            and not record.inherits_constructors
        )
        always = set.intersection(*constructors) if constructors else set()

        for member in record.members:
            if member.has_initializer:
                if complete and member.name in always:
                    results.append(
                        Finding(
                            record.member_file,
                            member.line,
                            class_name,
                            member.name,
                            "dead",
                        )
                    )
                continue

            if unparsed:
                continue
            leaves_default = not constructors or any(
                member.name not in names for names in constructors
            )
            if leaves_default and ALWAYS_DEFAULT_CONSTRUCTIBLE.match(member.type_name):
                results.append(
                    Finding(
                        record.member_file,
                        member.line,
                        class_name,
                        member.name,
                        "missing",
                    )
                )

    return sorted(results, key=lambda item: (item.file, item.line))


MESSAGES = {
    "dead": (
        "{class_name}::{member} has both an in-class initializer and a "
        "member-initializer-list entry in every constructor; the constructor "
        "wins, so the in-class initializer is dead"
    ),
    "missing": (
        "{class_name}::{member} has no in-class initializer and a construction "
        "path leaves it default-initialized"
    ),
}


def violations(files: list[tuple[str, str]]) -> list[str]:
    return [
        f"{item.file}:{item.line}: "
        + MESSAGES[item.kind].format(class_name=item.class_name, member=item.member)
        for item in findings(collect(files))
    ]


def violations_for_paths(paths: list[Path], root: Path) -> list[str]:
    return violations(
        [
            (path.relative_to(root).as_posix(), path.read_text(encoding="utf-8"))
            for path in paths
        ]
    )
