#!/usr/bin/env python3
"""Generate the one outward-facing document, derived from this repository's bytes.

A consuming repository must not read this repository's plans, reviews or
archive to learn what the framework offers. Those documents state status --
which gate is green, which commit landed, how far either side has got -- and a
consumer that transcribes status transcribes something it cannot verify and
that rots the moment this repository moves. The v1.18/v1.19 drift between the
framework's prose and a consumer's interface lock is exactly that failure.

So this repository publishes exactly one outward document,
``docs/PUBLIC-CONTRACT.md``, and every fact in it is read out of bytes here:
the published schemas under ``schema/``, the schema identities the
deployment module compiles from its own source, the wire tags the sources
write, the usage text and refusal strings the CLI entry points hold, and the
two fixture projects under ``examples/``.

Nothing about status, and nothing about a consumer's version, may enter the
output. The document says what this repository offers, not how far anyone has
got with it.

``--check`` regenerates and compares against the committed file, so the
committed bytes cannot drift from the generator.
"""

from __future__ import annotations

import argparse
import ast
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path


OUTPUT_RELATIVE_PATH = "docs/PUBLIC-CONTRACT.md"
GENERATOR_RELATIVE_PATH = "scripts/generate_public_contract.py"

SCHEMA_DIRECTORY = "schema"

# The header whose struct members enumerate one loaded deployment's
# authorities, and the schema that states what a project directory must
# declare.
DEPLOYMENT_DIRECTORY_HEADER = (
    "modules/deployment/source/deployment/project-directory.hpp"
)
PROJECT_DIRECTORY_SCHEMA = "schema/umbraflow-project-v3.schema.json"
SCRIPT_CONTRACT_HEADER = "modules/script/source/script/pure-data-program.hpp"
SCRIPT_CONTRACT_SOURCE = "modules/script/source/script/ffi/pure-data-program.cpp"
# The closed-graph runtime both program types share. The ceilings, the published
# contracts and the environment material live there because a pure program and a
# scoped one must agree on every one of them by construction rather than by
# comparison; only the wall-clock ceiling is each program type's own.
SCRIPT_RUNTIME_HEADER = "modules/script/source/script/ffi/program-runtime.hpp"
SCRIPT_RUNTIME_SOURCE = "modules/script/source/script/ffi/program-runtime.cpp"
PROJECT_PLUGIN_SOURCE = "modules/operator/source/operator/project-plugin.cpp"
FRAMEWORK_BUNDLE_SOURCE = "modules/task/source/task/framework-bundle.cpp"
FRAMEWORK_RUNTIME_DIRECTORY = "modules/task/runtime"
UNICODE_LICENSE = "modules/task/runtime/UNICODE-LICENSE.txt"

# The sources that hold framework schema identities as embedded JSON: the
# operator protocol the project's plugin answers, and the framework-format
# documents a project writes.
EMBEDDED_SCHEMA_SOURCES = (
    "modules/deployment/source/deployment/project-deployment.cpp",
    "modules/deployment/source/deployment/project-directory.cpp",
)

# Where the Operator mints an observed instance identity. The authority input's
# member set is the whole of what the Operator binds, so it is read from the
# function that writes those bytes rather than restated.
LEDGER_SOURCE = "modules/operator/source/operator/ledger.cpp"
AUTHORITY_BYTES_FUNCTION = "observedInstanceAuthorityBytes"

# Where the Tool Runtime states itself. Each of these holds one half of what a
# consumer driving a Tool call has to know, and the generator reads every
# published fact out of them rather than restating it here: the call-state
# vocabulary and its terminal split, the identity preimages and the
# root-idempotency relation, the durable record's stored DDL, the assembly the
# protocol identity is taken over, and the answer envelope a scoped script sees.
TOOL_RUNTIME_SOURCE = "modules/operator/source/operator/tool-runtime.cpp"
TOOL_INVOCATION_SOURCE = "modules/operator/source/operator/tool-invocation.cpp"
TOOL_RUNTIME_LUAU = "modules/task/runtime/tools.luau"
DEPLOYMENT_PROTOCOL_SOURCE = (
    "modules/deployment/source/deployment/project-deployment.cpp"
)

# One column of a ``CREATE TABLE x(...)`` DDL constant: the first identifier of
# a top-level element. Table constraints are excluded by name, because a
# constraint is not a column and a consumer told otherwise would look for one.
DDL_TABLE_CONSTRAINT = ("primary", "unique", "check", "foreign", "constraint")

OBSERVATION_PROPOSAL_SCHEMA = "schema/umbraflow-project-observation-proposal-v1.schema.json"
OBSERVATION_SCHEMA = "schema/umbraflow-project-observation-v1.schema.json"

# The publisher script that writes the release manifest project upgrade parses.
# Its constants are the shape authority; the generator extracts the
# wire tag and the member tuples from these bytes rather than restating them.
RELEASE_SOURCE = "scripts/publish_release.py"

# Exit-code enumerations, each the whole convention for one executable.
EXIT_CODE_ENUMS = (
    ("umbra-flow", "modules/cli/source/cli/cli-result.hpp", "ExitCode"),
    ("project", "modules/project/source/project/command.hpp", "ProjectExitCode"),
)

# Where the CLI surface is written: the usage text a consumer reads and the
# refusals it will see. Ordered so the document reads outward-in -- the two
# shipped binaries a consumer invokes, then the loader and the kit behind them.
#
# A scope of () is the whole file, which is right where every refusal in it is
# about a project directory. Named scopes are for a file that also serves
# commands a consumer never runs against its own directory: `umbra-flow` has
# eight subcommands and seven of them reach a live target, so listing every
# refusal in its argument parser would bury the two `open` produces.
CLI_SURFACE_SOURCES = (
    (
        "umbra-flow open",
        "modules/cli/source/cli/args.cpp",
        ("parseOpenArguments", "requirePath"),
    ),
    # invoke is the verb that starts a Tool call, so a consumer driving the Tool
    # Runtime from outside reads these and not open's. Its scope names the
    # argument helpers it reaches as well as its own parser, because the actor
    # rules -- which material each transport carries, and that an actor is never
    # inferred -- are stated in messages those helpers and the parser produce
    # together.
    (
        "umbra-flow invoke",
        "modules/cli/source/cli/args.cpp",
        (
            "parseInvokeArguments",
            "requirePath",
            "parseWindowHandle",
            "parseUnsigned",
            "parseDurationCount",
        ),
    ),
    (
        "umbra-flow-conformance",
        "modules/conformance/source/conformance/suite-run.cpp",
        (),
    ),
    ("project", "modules/project/source/project/command.cpp", ()),
    (
        "project upgrade",
        "modules/project/source/project/release-bundle.cpp",
        (),
    ),
    (
        "project (process transport on Windows)",
        "modules/project/source/project/platform/process-run-windows.cpp",
        (),
    ),
    (
        "project (process transport on POSIX)",
        "modules/project/source/project/platform/process-run-posix.cpp",
        (),
    ),
    ("project (entry point)", "entry/project/main.cpp", ()),
    (
        "project directory loader",
        "modules/deployment/source/deployment/project-directory.cpp",
        (),
    ),
    (
        "deployment authorities",
        "modules/deployment/source/deployment/project-deployment.cpp",
        (),
    ),
    ("project kit", "modules/project/source/project/project-kit.cpp", ()),
)

# The usage text of the commands a consumer invokes against its own directory,
# each named by the definition that holds it.
CLI_USAGE_DEFINITIONS = (
    ("modules/cli/source/cli/args.cpp", "openUsageText"),
    ("modules/cli/source/cli/args.cpp", "invokeUsageText"),
    ("modules/conformance/source/conformance/suite-run.cpp", "k_usageText"),
    ("modules/project/source/project/command.cpp", "projectUsageText"),
)

EXAMPLES_DIRECTORY = "examples"
ROOT_CMAKE = "CMakeLists.txt"

# Every ``umbraflow-<name>/v<n>`` literal: how a wire tag is spelled on both
# sides of this boundary.
WIRE_TAG = re.compile(r"umbraflow-[a-z0-9-]+/v[0-9]+")

# A wire tag a JSON Schema pins as the only accepted value of a member. A
# schema that pins a tag is the framework's statement of ownership over
# documents carrying it.
PINNED_WIRE_TAG = re.compile(
    r"\"const\"\s*:\s*\"(umbraflow-[a-z0-9-]+/v[0-9]+)\""
)

# A raw string literal, which is how a module source carries an embedded JSON
# Schema. The d-char-sequence is captured so the closing token is matched
# exactly, as [lex.string] requires.
RAW_STRING = re.compile(r"R\"([A-Za-z0-9_]{0,16})\((?P<body>.*?)\)\1\"", re.DOTALL)

# The version token an identity carries. An ``$id`` ending in ``/v<n>`` states
# it there; a published file whose ``$id`` does not states it in its name.
ID_VERSION = re.compile(r"/v([0-9]+)$")
FILE_VERSION = re.compile(r"-v([0-9]+)\.schema\.json$")

# An ordinary string literal, escapes included.
STRING_LITERAL = re.compile(r"\"(?:\\.|[^\"\\\n])*\"")

# One more adjacent string literal, which the translation phase concatenates
# into the preceding one. A refusal spanning six source lines is one message.
ADJACENT_LITERAL = re.compile(r"\s*(\"(?:\\.|[^\"\\\n])*\")")

# The calls that produce a message a consumer reads. ``refuse`` and
# ``complain`` are the loader's own helpers over ``fail``; ``invalid`` is the
# CLI argument parser's. ``std::cerr <<`` is the entry points writing straight
# to the stream.
MESSAGE_CALL = re.compile(r"\b(?:refuse|fail|invalid|complain)\s*\(|std::cerr\s*<<")

# The label a module source compiles one embedded schema under. It is the
# identity's own path suffix, so the label is what ties a compile site to a
# ``$id`` without either restating the other.
COMPILE_LABEL = re.compile(r"(?:\.label\s*=|compile\(\s*)\s*\"([^\"]+)\"")

# The opening brace of one ``PureModuleBinding{...}`` row in the framework
# bundle source. The lookbehind excludes ``InternalPureModuleBinding{``: that
# is a distinct table for names no Project-authored module can ever resolve,
# and without it the substring ``PureModuleBinding{`` inside
# ``InternalPureModuleBinding{`` would match too. The row's own field count is
# not baked into this pattern -- ``braced_span`` reads whatever fields the
# struct currently declares.
PURE_MODULE_BINDING = re.compile(r"(?<!Internal)PureModuleBinding\{")

# The opening brace of one ``ScopedModuleBinding{...}`` row in the same source.
# Scoped modules are publicly named SDK surface like the pure ones, but they are
# a DIFFERENT contract -- they load only inside a ScopedToolProgram -- so they
# are read by their own pattern into their own section rather than being folded
# into the pure list.
SCOPED_MODULE_BINDING = re.compile(r"\bScopedModuleBinding\{")

# A struct member declared as one of the Operator's schema-bearing authorities.
# The type ending decides, not the member name: an authority is an ``...Owner``
# or a set of ``...Schemas``, and the registration beside them is a verified
# document rather than an authority.
DEPLOYMENT_AUTHORITY = re.compile(
    r"operator_runtime::(\w+(?:Owner|Schemas))\s+(\w+)\s*(?:\{\s*\}\s*)?;"
)

# One conformance run this repository registers against one of its own fixture
# directories, and the CTest name that run carries.
CONFORMANCE_RUN = re.compile(
    r"uf_add_conformance_run\(\s*PROJECT\s+([A-Za-z0-9-]+)\s+"
    r"DIRECTORY\s+\"\$\{(\w+)\}\"",
    re.DOTALL,
)
STAGED_PROJECT_VARIABLE = re.compile(
    r"set\((UF_STAGED_\w+_PROJECT)\s+\"\$\{UF_STAGED_EXAMPLE_ROOT\}/([A-Za-z0-9-]+)\"\)"
)

# A member name the authority-input writer appends, or one of the braces around
# it. The braces are captured too, so a member of the nested world_scope object
# is not read as a top-level one.
AUTHORITY_FRAGMENT = re.compile(r"([{}])|\"(\w+)\":")


@dataclass(frozen=True)
class SchemaIdentity:
    """One schema this repository publishes as exact bytes under a stable $id."""

    schema_id: str
    origin: str
    version: str
    wire_tag: str
    shape: str


@dataclass(frozen=True)
class RefusalSource:
    """Every distinct message one source file can print, in source order."""

    label: str
    path: str
    messages: tuple[str, ...]


def read(root: Path, relative: str) -> str:
    return (root / relative).read_text(encoding="utf-8")


def unescape(literal: str) -> str:
    """The value of one C++ string literal, as the program holds it."""
    return (
        literal[1:-1]
        .replace('\\"', '"')
        .replace("\\n", "\n")
        .replace("\\t", "\t")
        .replace("\\\\", "\\")
    )


def strip_comments_and_raw_strings(text: str) -> str:
    """C++ source with comments and raw strings removed, ordinary literals kept.

    Comments are removed because a message only a stale comment spells is not
    a message any consumer sees. Raw strings are removed because they carry
    embedded JSON Schemas, whose quoted member names are not refusals; the
    schemas are read separately, from the same bytes.
    """
    kept: list[str] = []
    index = 0
    size = len(text)
    while index < size:
        raw = RAW_STRING.match(text, index)
        if raw is not None:
            kept.append('""')
            index = raw.end()
            continue

        character = text[index]
        following = text[index + 1] if index + 1 < size else ""
        if character == '"':
            literal = STRING_LITERAL.match(text, index)
            if literal is None:
                index += 1
                continue
            kept.append(literal.group(0))
            index = literal.end()
            continue
        if character == "/" and following == "/":
            newline = text.find("\n", index)
            index = size if newline < 0 else newline
            continue
        if character == "/" and following == "*":
            end = text.find("*/", index + 2)
            index = size if end < 0 else end + 2
            continue
        kept.append(character)
        index += 1
    return "".join(kept)


def balanced_span(text: str, open_parenthesis: int) -> str:
    """The text inside the parenthesis pair opening at open_parenthesis."""
    depth = 0
    index = open_parenthesis
    while index < len(text):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return text[open_parenthesis + 1 : index]
        index += 1
    return text[open_parenthesis + 1 :]


def braced_span(text: str, open_brace: int) -> str:
    """The text inside the brace pair opening at open_brace.

    Same shape as balanced_span, but for the ``{...}`` a struct's braced
    initialiser opens rather than the ``(...)`` a call opens.
    """
    depth = 0
    index = open_brace
    while index < len(text):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace + 1 : index]
        index += 1
    return text[open_brace + 1 :]


def concatenated_literals(span: str) -> list[str]:
    """Every run of adjacent string literals in span, each as one value."""
    values: list[str] = []
    position = 0
    while True:
        first = STRING_LITERAL.search(span, position)
        if first is None:
            return values
        parts = [first.group(0)]
        end = first.end()
        while True:
            following = ADJACENT_LITERAL.match(span, end)
            if following is None:
                break
            parts.append(following.group(1))
            end = following.end()
        values.append("".join(unescape(part) for part in parts))
        position = end


def single_line(value: str) -> str:
    """One message on one line, so a table row and a code line hold it."""
    return " ".join(value.split())


def function_span(text: str, name: str) -> str:
    """One definition's whole body, from its own name to its closing brace.

    The name must be the one a definition introduces -- ``auto <name>(`` for a
    function, ``auto <name> =`` for a constant -- so a call site earlier in the
    file is not read as the definition.
    """
    introduction = re.search(rf"\bauto\s+{re.escape(name)}\s*[(=]", text)
    if introduction is None:
        raise SystemExit(f"{name}: no definition introduces this name any more")
    opening = text.find("{", introduction.end())
    if opening < 0:
        return text[introduction.end() :]
    depth = 0
    index = opening
    while index < len(text):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[introduction.end() : index]
        index += 1
    return text[introduction.end() :]


def messages_in(text: str, scope: tuple[str, ...]) -> list[str]:
    """Every distinct message the calls in text can print, in source order.

    An empty scope reads the whole file. A named scope reads only those
    definitions, for a file that also serves commands a consumer never runs.
    """
    code = strip_comments_and_raw_strings(text)
    spans = (
        [code]
        if not scope
        else [function_span(code, name) for name in scope]
    )
    found: list[str] = []
    for span_text in spans:
        for call in MESSAGE_CALL.finditer(span_text):
            if call.group(0).startswith("std::cerr"):
                terminator = span_text.find(";", call.end())
                span = span_text[
                    call.end() : len(span_text) if terminator < 0 else terminator
                ]
            else:
                opening = span_text.find("(", call.start())
                if opening < 0:
                    continue
                span = balanced_span(span_text, opening)
            for value in concatenated_literals(span):
                collapsed = single_line(value)
                # A message is a sentence. A one-word literal in a call's
                # argument list is a member name, a role or a digest prefix,
                # and printing it here would bury the refusals a consumer is
                # reading for.
                if len(collapsed.split()) < 3:
                    continue
                if collapsed not in found:
                    found.append(collapsed)
    return found


def definition_literal(text: str, name: str) -> str:
    """The concatenated string literal one usage-text definition holds."""
    values = concatenated_literals(function_span(text, name))
    if not values:
        raise SystemExit(f"{name}: holds no string literal any more")
    return values[0]


def usage_lines(usage: str) -> list[str]:
    """The invocation shapes a usage text states under its Usage: heading.

    Relative indentation is preserved and only the block's own common indent is
    removed, because a shape wrapped over four lines is ONE invocation: flushing
    its continuations left would publish four.
    """
    collected: list[str] = []
    inline: list[str] = []
    collecting = False
    for line in usage.splitlines():
        stripped = line.strip()
        if stripped.lower().startswith("usage:"):
            collecting = True
            remainder = stripped[len("usage:") :].strip()
            if remainder:
                inline.append(remainder)
            continue
        if collecting:
            if not stripped:
                break
            collected.append(line.rstrip())
    if not collected:
        return inline
    common = min(len(line) - len(line.lstrip(" ")) for line in collected)
    return inline + [line[common:] for line in collected]


def embedded_schema_id(text: str, label: str) -> str:
    """The ``$id`` of the embedded framework schema one compile label names.

    The label is the identity's own path suffix, which is what ties a compile
    site to an ``$id`` without either restating the other -- the same relation
    section 1.3 is built on.
    """
    for match in RAW_STRING.finditer(text):
        found = re.search(r'"\$id"\s*:\s*"([^"]+)"', match.group("body"))
        if found is not None and found.group(1).endswith(f"/{label}"):
            return found.group(1)
    raise SystemExit(f"no embedded schema in this source carries the label {label}")


def one_message_in(text: str, definition: str, needle: str) -> str:
    """The one message a definition produces that names ``needle``.

    Exactly one, deliberately: a second message naming the same thing means the
    refusal published here is no longer the only one a consumer can see, and a
    generator that picked the first would publish half a contract.
    """
    found = [
        message for message in messages_in(text, (definition,)) if needle in message
    ]
    if len(found) != 1:
        raise SystemExit(
            f"{definition}: produces {len(found)} messages naming {needle}, not one"
        )
    return found[0]


def ddl_columns(text: str, constant: str) -> list[str]:
    """The column names one ``CREATE TABLE`` DDL constant declares, in order.

    The DDL is the stored text this schema's identity is taken from, so reading
    the column set out of it is the same read the database performs. A table
    constraint is skipped by its leading keyword rather than by position: the
    constraints sit at the end today and a column added after one would be lost
    by a positional rule.
    """
    span = "".join(concatenated_literals(function_span(text, constant)))
    opening = span.find("(")
    if opening < 0:
        raise SystemExit(f"{constant}: holds no CREATE TABLE body any more")
    depth = 0
    element = ""
    elements: list[str] = []
    for character in span[opening:]:
        if character == "(":
            depth += 1
            if depth == 1:
                continue
        elif character == ")":
            depth -= 1
            if depth == 0:
                break
        if depth == 1 and character == ",":
            elements.append(element)
            element = ""
            continue
        element += character
    elements.append(element)

    columns: list[str] = []
    for one in elements:
        # A column is a leading identifier followed by a type; a table
        # constraint is a keyword followed immediately by its own parenthesis.
        # Both tests are applied, because CHECK and UNIQUE are also legal column
        # names in SQLite only when quoted, which this DDL never does.
        leading = re.match(r"\s*([A-Za-z_][A-Za-z0-9_]*)(\s|\()", one)
        if leading is None:
            continue
        name = leading.group(1)
        if name.lower() in DDL_TABLE_CONSTRAINT or leading.group(2) == "(":
            continue
        columns.append(name)
    if not columns:
        raise SystemExit(f"{constant}: declares no column any more")
    return columns


def tool_call_states(root: Path) -> list[tuple[str, bool]]:
    """Every durable call-state wire name, and which half of the split it is in.

    Both halves are read: the names from the one table that maps a state to its
    wire spelling, and the split from the one predicate that decides whether a
    state carries an outcome. They are then required to cover the same set, so a
    state added to one and not the other fails here rather than being published
    on the strength of whichever list was read first.
    """
    text = read(root, TOOL_RUNTIME_SOURCE)
    span = function_span(text, "k_toolCallStateNames")
    rows = re.findall(r"ToolCallState::(\w+)\s*,\s*\n?\s*\"([a-z_]+)\"", span)
    if not rows:
        raise SystemExit(f"{TOOL_RUNTIME_SOURCE}: the call-state table is unreadable")

    outcome = function_span(text, "toolCallStateHasOutcome")
    before, marker, after = outcome.partition("return false;")
    if not marker:
        raise SystemExit(
            f"{TOOL_RUNTIME_SOURCE}: toolCallStateHasOutcome no longer splits on"
            " a false arm"
        )
    without = set(re.findall(r"ToolCallState::(\w+)", before))
    carrying = set(re.findall(r"ToolCallState::(\w+)", after.split("return true;")[0]))
    if not without or not carrying or (without & carrying):
        raise SystemExit(
            f"{TOOL_RUNTIME_SOURCE}: the terminal split is empty or overlapping"
        )
    named = {name for name, _wire in rows}
    if named != (without | carrying):
        raise SystemExit(
            f"{TOOL_RUNTIME_SOURCE}: the call-state table and the terminal split"
            f" disagree: table={sorted(named)},"
            f" split={sorted(without | carrying)}"
        )
    return [(wire, name in carrying) for name, wire in rows]


def tool_answer_envelope(root: Path) -> dict[str, object]:
    """The answer a scoped script receives, read out of the module that checks it."""
    text = read(root, TOOL_RUNTIME_LUAU)
    frozen = re.search(r"local k_states = table\.freeze\(\{(.*?)\}\)", text, re.DOTALL)
    if frozen is None:
        raise SystemExit(f"{TOOL_RUNTIME_LUAU}: publishes no delivery vocabulary")
    states = re.findall(r'"([a-z_]+)"', frozen.group(1))

    required = re.findall(r'rawget\(answer,\s*"(\w+)"\)', text)
    optional = re.findall(r'rawget\(checkedAnswer\(answer\),\s*"(\w+)"\)', text)
    members: list[str] = []
    for member in required + optional:
        if member not in members:
            members.append(member)
    if not members:
        raise SystemExit(f"{TOOL_RUNTIME_LUAU}: reads no answer member")

    # The two members an accessor answers as a @umbraflow/result envelope
    # rather than a value: absence is a failure of the request, not a value.
    enveloped = re.findall(r'resultModule\.err\(\s*"([\w.]+)"', text)
    return {
        "states": states,
        "members": members,
        "absent_result_codes": enveloped,
    }


def tool_root_identity(root: Path) -> dict[str, object]:
    """The root request preimage and the relation two of them stand in."""
    text = read(root, TOOL_INVOCATION_SOURCE)
    tag = re.search(
        r"k_rootPreimageTag\s*=\s*std::string_view\{\s*\n?\s*\"([^\"]+)\"",
        text,
    )
    call_tag = re.search(
        r"k_callPreimageTag\s*=\s*std::string_view\{\s*\n?\s*\"([^\"]+)\"",
        text,
    )
    if tag is None or call_tag is None:
        raise SystemExit(f"{TOOL_INVOCATION_SOURCE}: an identity domain tag is gone")

    material = function_span(text, "rootIdentityMaterial")
    parts = re.findall(r"appendIdentity\w*\(\s*material,\s*(\w+(?:\.\w+\(\))?)", material)
    relations = re.findall(
        r"RootRequestRelation::(\w+)",
        function_span(text, "ToolRootRequestIdentity::relationTo"),
    )
    if not parts or not relations:
        raise SystemExit(
            f"{TOOL_INVOCATION_SOURCE}: the root identity is no longer derived here"
        )
    return {
        "root_tag": tag.group(1),
        "call_tag": call_tag.group(1),
        "root_parts": parts,
        "relations": relations,
    }


def tool_protocol_material(root: Path) -> list[str]:
    """The members the protocol identity's preimage carries, in canonical order."""
    text = read(root, DEPLOYMENT_PROTOCOL_SOURCE)
    span = function_span(text, "currentToolRuntimeProtocolMaterial")
    members = re.findall(r'\{"(\w+)",', span)
    if not members:
        raise SystemExit(
            f"{DEPLOYMENT_PROTOCOL_SOURCE}: the protocol material carries no member"
        )
    return sorted(set(members))


def version_of(schema_id: str, filename: str) -> str:
    in_id = ID_VERSION.search(schema_id)
    if in_id is not None:
        return f"v{in_id.group(1)}"
    in_name = FILE_VERSION.search(filename)
    if in_name is not None:
        return f"v{in_name.group(1)}"
    return "unversioned"


def shape_of(document: dict[str, object]) -> str:
    """What a document requires, or what it is one of when it requires nothing."""
    required = document.get("required")
    if isinstance(required, list):
        return ", ".join(f"`{member}`" for member in required)
    one_of = document.get("oneOf")
    if isinstance(one_of, list):
        branches = [
            str(branch.get("$ref", "")).removeprefix("#/$defs/")
            for branch in one_of
            if isinstance(branch, dict)
        ]
        return "one of " + ", ".join(f"`{branch}`" for branch in branches if branch)
    reference = document.get("$ref")
    if isinstance(reference, str):
        return f"`{reference.removeprefix('#/$defs/')}`"
    return "no top-level requirement"


def published_schemas(root: Path) -> list[SchemaIdentity]:
    """Every schema this repository publishes as exact bytes under a stable $id."""
    directory = root / SCHEMA_DIRECTORY
    identities: list[SchemaIdentity] = []
    for path in sorted(directory.glob("*.json")):
        text = path.read_text(encoding="utf-8")
        document = json.loads(text)
        schema_id = document.get("$id")
        if not isinstance(schema_id, str):
            raise SystemExit(f"{path}: declares no string $id")
        tags = sorted(set(PINNED_WIRE_TAG.findall(text)))
        identities.append(
            SchemaIdentity(
                schema_id=schema_id,
                origin=f"{SCHEMA_DIRECTORY}/{path.name}",
                version=version_of(schema_id, path.name),
                wire_tag=tags[0] if tags else "",
                shape=shape_of(document),
            )
        )
    if not identities:
        raise SystemExit(f"{directory}: publishes no schema")
    return identities


def embedded_schema_documents(root: Path) -> list[tuple[str, str, dict[str, object]]]:
    """Every schema the deployment module compiles from its own source bytes."""
    documents: list[tuple[str, str, dict[str, object]]] = []
    for relative in EMBEDDED_SCHEMA_SOURCES:
        text = read(root, relative)
        for raw in RAW_STRING.finditer(text):
            body = raw.group("body")
            if '"$id"' not in body:
                continue
            document = json.loads(body)
            schema_id = document.get("$id")
            if isinstance(schema_id, str):
                documents.append((schema_id, relative, document))
    if not documents:
        raise SystemExit("no module source carries an embedded schema any more")
    return sorted(documents, key=lambda entry: entry[0])


def reference_targets(root: Path) -> set[str]:
    """Every absolute $id some schema in this repository resolves by $ref."""
    targets: set[str] = set()
    texts = [
        (root / SCHEMA_DIRECTORY / path.name).read_text(encoding="utf-8")
        for path in sorted((root / SCHEMA_DIRECTORY).glob("*.json"))
    ]
    texts.extend(read(root, relative) for relative in EMBEDDED_SCHEMA_SOURCES)
    for text in texts:
        for reference in re.finditer(r"\"\$ref\"\s*:\s*\"(https?://[^\"#]+)", text):
            targets.add(reference.group(1))
    return targets


def written_wire_tags(root: Path) -> set[str]:
    """Every wire tag this repository's first-party sources still write.

    ``entry/`` counts as well as ``modules/``: a tag only an entry executable
    spells is still a tag a consumer sees. ``entry/workbench`` is excluded
    because it is vendored.
    """
    tags: set[str] = set()
    patterns = (
        "modules/*/source/**/*.cpp",
        "modules/*/source/**/*.hpp",
        "entry/*/*.cpp",
    )
    for pattern in patterns:
        for path in root.glob(pattern):
            if "external" in path.parts:
                continue
            tags.update(WIRE_TAG.findall(path.read_text(encoding="utf-8")))
    for path in sorted((root / SCHEMA_DIRECTORY).glob("*.json")):
        tags.update(WIRE_TAG.findall(path.read_text(encoding="utf-8")))
    # A tag only the publisher spells is still a tag project upgrade sees, on the
    # same terms as a tag only an entry executable spells.
    tags.update(WIRE_TAG.findall(read(root, RELEASE_SOURCE)))
    if not tags:
        raise SystemExit("no source writes an umbraflow-<name>/v<n> tag any more")
    return tags


def python_string_tuple(source: str, name: str) -> tuple[str, ...]:
    """Every string literal in a flat ``NAME = ("a", "b")`` tuple."""
    start = source.index(f"{name} = (")
    end = source.index(")", start)
    return tuple(re.findall(r'"([^"]+)"', source[start:end]))


def python_string_constant(source: str, name: str) -> str:
    """The value of a plain ``NAME = "value"`` string constant."""
    match = re.search(rf'{name}\s*=\s*"([^"]+)"', source)
    if match is None:
        raise SystemExit(f"{RELEASE_SOURCE} must define {name}")
    return match.group(1)


def release_manifest_facts(root: Path) -> dict[str, object]:
    """The release manifest's tag, members and vocabulary, read out of the
    publisher script's bytes."""
    source = read(root, RELEASE_SOURCE)
    tag = python_string_constant(source, "RELEASE_MANIFEST_SCHEMA")
    if tag not in WIRE_TAG.findall(source):
        raise SystemExit(
            f"{RELEASE_SOURCE} must write the release tag its schema constant pins"
        )
    return {
        "tag": tag,
        "members": python_string_tuple(source, "RELEASE_MANIFEST_MEMBERS"),
        "artifact_members": python_string_tuple(
            source, "RELEASE_ARTIFACT_MEMBERS"
        ),
        "contract_versions": python_string_tuple(
            source, "RELEASE_MANIFEST_CONTRACT_VERSIONS"
        ),
        "binaries": python_string_tuple(source, "RELEASE_BINARIES"),
        "payload_patterns": python_string_tuple(
            source, "RELEASE_PAYLOAD_PATTERNS"
        ),
    }


def compile_labels(root: Path) -> dict[str, str]:
    """The label each embedded identity's own source compiles it under.

    An identity's label is its path suffix, which is how a compile site names
    the document without restating the ``$id``. The mapping is what lets an
    identity that fits none of the ownership categories still be reported
    with the byte that ties it to a caller, rather than as a bare gap.
    """
    labels: dict[str, str] = {}
    for relative in EMBEDDED_SCHEMA_SOURCES:
        code = strip_comments_and_raw_strings(read(root, relative))
        for label in COMPILE_LABEL.findall(code):
            labels[label] = relative
    return labels


def deployment_authorities(root: Path) -> list[tuple[str, str]]:
    """The schema-bearing authorities one loaded deployment carries."""
    header = read(root, DEPLOYMENT_DIRECTORY_HEADER)
    found = DEPLOYMENT_AUTHORITY.findall(header)
    if not found:
        raise SystemExit(
            f"{DEPLOYMENT_DIRECTORY_HEADER}: declares no operator authority member"
        )
    ordered: list[tuple[str, str]] = []
    for type_name, member in found:
        if (type_name, member) not in ordered:
            ordered.append((type_name, member))
    return ordered


def exit_code_conventions(root: Path) -> list[tuple[str, list[tuple[str, str]]]]:
    """Each executable's whole exit-code vocabulary, read off its enumeration."""
    conventions: list[tuple[str, list[tuple[str, str]]]] = []
    for binary, relative, enumeration in EXIT_CODE_ENUMS:
        text = read(root, relative)
        declaration = re.search(
            rf"enum class {enumeration}\s*:\s*\w+\s*\{{(?P<body>[^}}]*)\}}", text
        )
        if declaration is None:
            raise SystemExit(f"{relative}: declares no enum class {enumeration}")
        codes = [
            (name, value)
            for name, value in re.findall(
                r"(\w+)\s*=\s*(\d+)", declaration.group("body")
            )
        ]
        conventions.append((binary, codes))
    return conventions


def authority_input_members(root: Path) -> tuple[str, list[str]]:
    """The wire tag and top-level member set the Operator binds an identity through.

    Depth is tracked because the writer appends the nested ``world_scope``
    object's own members from the same string literals; a flat scan would
    report them beside the top-level ones as if they were siblings.
    """
    text = read(root, LEDGER_SOURCE)
    start = text.find(AUTHORITY_BYTES_FUNCTION)
    if start < 0:
        raise SystemExit(f"{LEDGER_SOURCE}: no {AUTHORITY_BYTES_FUNCTION} any more")
    body = text[start : text.find("\n        [[nodiscard]]", start + 1)]

    # Only the literal text the function appends, in append order: the C++
    # braces around the function itself are not object braces, and counting
    # them would put every member at the wrong depth.
    written = "".join(
        unescape(literal) for literal in STRING_LITERAL.findall(body)
    )
    members: list[str] = []
    depth = 0
    for brace, member in AUTHORITY_FRAGMENT.findall(written):
        if brace == "{":
            depth += 1
            continue
        if brace == "}":
            depth -= 1
            continue
        if depth == 1 and member not in members:
            members.append(member)
    tags = sorted(set(WIRE_TAG.findall(written)))
    if not members or not tags:
        raise SystemExit(
            f"{LEDGER_SOURCE}: {AUTHORITY_BYTES_FUNCTION} no longer writes a "
            "tagged member set"
        )
    return tags[0], members


def item_requirements(document: dict[str, object], member: str) -> list[str]:
    """The required members of one array-valued property's items."""
    properties = document.get("properties")
    if not isinstance(properties, dict):
        return []
    array = properties.get(member)
    if not isinstance(array, dict):
        return []
    items = array.get("items")
    if not isinstance(items, dict):
        return []
    required = items.get("required")
    return [str(name) for name in required] if isinstance(required, list) else []


def item_pattern(document: dict[str, object], member: str, name: str) -> str:
    """The pattern one item member is held to, or an empty string."""
    properties = document.get("properties")
    if not isinstance(properties, dict):
        return ""
    array = properties.get(member)
    if not isinstance(array, dict):
        return ""
    items = array.get("items")
    if not isinstance(items, dict):
        return ""
    item_properties = items.get("properties")
    if not isinstance(item_properties, dict):
        return ""
    target = item_properties.get(name)
    if not isinstance(target, dict):
        return ""
    pattern = target.get("pattern")
    return str(pattern) if isinstance(pattern, str) else ""


def definition(document: dict[str, object], name: str) -> dict[str, object]:
    definitions = document.get("$defs")
    if not isinstance(definitions, dict):
        raise SystemExit(f"{PROJECT_DIRECTORY_SCHEMA}: holds no $defs")
    found = definitions.get(name)
    if not isinstance(found, dict):
        raise SystemExit(f"{PROJECT_DIRECTORY_SCHEMA}: holds no $defs/{name}")
    return found


def tool_declaration_bound(root: Path) -> int:
    """The ceiling a deployment's inline Tool declarations are held to."""
    directory_schema = json.loads(read(root, PROJECT_DIRECTORY_SCHEMA))
    deployment = definition(directory_schema, "Deployment")
    properties = deployment.get("properties")
    if not isinstance(properties, dict):
        raise SystemExit(f"{PROJECT_DIRECTORY_SCHEMA}: Deployment states no properties")
    tools = properties.get("tools")
    if not isinstance(tools, dict) or not isinstance(tools.get("maxItems"), int):
        raise SystemExit(
            f"{PROJECT_DIRECTORY_SCHEMA}: tools states no maxItems any more"
        )
    return int(tools["maxItems"])


def fixture_projects(root: Path) -> list[tuple[str, str, dict[str, object]]]:
    """Each fixture project directory, its CTest name, and its own manifest."""
    cmake = read(root, ROOT_CMAKE)
    staged = {
        variable: directory
        for variable, directory in STAGED_PROJECT_VARIABLE.findall(cmake)
    }
    projects: list[tuple[str, str, dict[str, object]]] = []
    for name, variable in CONFORMANCE_RUN.findall(cmake):
        directory = staged.get(variable)
        if directory is None:
            raise SystemExit(f"{ROOT_CMAKE}: {variable} names no staged directory")
        manifest = (
            root / EXAMPLES_DIRECTORY / directory / "umbraflow-project.json"
        ).read_text(encoding="utf-8")
        projects.append(
            (f"conformance-{name}", f"{EXAMPLES_DIRECTORY}/{directory}", json.loads(manifest))
        )
    if not projects:
        raise SystemExit(f"{ROOT_CMAKE}: registers no conformance run any more")
    return projects


def refusal_sources(root: Path) -> list[RefusalSource]:
    return [
        RefusalSource(
            label=label,
            path=relative,
            messages=tuple(messages_in(read(root, relative), scope)),
        )
        for label, relative, scope in CLI_SURFACE_SOURCES
    ]


def table(headings: list[str], rows: list[list[str]]) -> list[str]:
    lines = ["| " + " | ".join(headings) + " |"]
    lines.append("|" + "|".join(" --- " for _ in headings) + "|")
    for row in rows:
        lines.append("| " + " | ".join(row) + " |")
    return lines


def cell(value: str) -> str:
    """One table cell, with the two characters a table row cannot hold escaped."""
    return value.replace("\\", "\\\\").replace("|", "\\|")


def integer_expression(expression: str, constants: dict[str, int]) -> int:
    """Evaluate the deliberately tiny integer subset used by limit constants."""
    if "duration_cast" in expression:
        # The environment material takes its wall-clock ceiling as a parameter,
        # because each program type sets its own; the pure environment this
        # contract describes passes k_pureRuntimeCeiling.
        seconds = constants["k_pureRuntimeCeiling"]
        return seconds * 1000
    normalized = expression.replace("PureDataProgram::", "")
    normalized = re.sub(
        r"MonotonicInstant::Duration\{(.*)\}", r"(\1)", normalized, flags=re.DOTALL
    )
    normalized = re.sub(r"std::(?:size_t|uint64)\{([^{}]+)\}", r"(\1)", normalized)
    normalized = re.sub(r"uint64\{([^{}]+)\}", r"(\1)", normalized)
    normalized = re.sub(r"std::chrono::seconds\{([^{}]+)\}", r"(\1)", normalized)
    normalized = re.sub(r"static_cast<int>\((.*)\)", r"(\1)", normalized, flags=re.DOTALL)
    normalized = normalized.replace("'", "")
    normalized = re.sub(r"(?<=\d)[UuLl]+", "", normalized)
    for name, value in sorted(constants.items(), key=lambda item: -len(item[0])):
        normalized = re.sub(rf"\b{re.escape(name)}\b", str(value), normalized)

    node = ast.parse(normalized.strip(), mode="eval")

    def visit(current: ast.AST) -> int:
        if isinstance(current, ast.Expression):
            return visit(current.body)
        if isinstance(current, ast.Constant) and isinstance(current.value, int):
            return current.value
        if isinstance(current, ast.BinOp) and isinstance(
            current.op, (ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Div)
        ):
            left = visit(current.left)
            right = visit(current.right)
            if isinstance(current.op, ast.Add):
                return left + right
            if isinstance(current.op, ast.Sub):
                return left - right
            if isinstance(current.op, ast.Mult):
                return left * right
            return left // right
        raise SystemExit(f"unsupported C++ limit expression: {expression.strip()}")

    return visit(node)


def script_runtime_contract(root: Path) -> dict[str, object]:
    """The public Luau environment surface, extracted from its implementation."""
    header = read(root, SCRIPT_CONTRACT_HEADER)
    runtime_header = read(root, SCRIPT_RUNTIME_HEADER)
    runtime_source = read(root, SCRIPT_RUNTIME_SOURCE)
    source = read(root, SCRIPT_CONTRACT_SOURCE)
    framework_bundle = read(root, FRAMEWORK_BUNDLE_SOURCE)
    read(root, UNICODE_LICENSE)
    constant_expressions: dict[str, str] = {}
    pattern = re.compile(r"(?:static\s+)?constexpr\s+auto\s+(k_\w+)\s*=\s*(.*?);", re.DOTALL)
    for text in (header, runtime_header, runtime_source, source):
        for name, expression in pattern.findall(text):
            constant_expressions[name] = expression

    constants: dict[str, int] = {}
    unresolved = dict(constant_expressions)
    while unresolved:
        progressed = False
        for name, expression in list(unresolved.items()):
            try:
                constants[name] = integer_expression(expression, constants)
            except (KeyError, SyntaxError, SystemExit):
                continue
            del unresolved[name]
            progressed = True
        if not progressed:
            break

    material = function_span(runtime_source, "sharedEnvironmentMaterial")
    limit_rows: list[tuple[str, int]] = []
    for name, expression in re.findall(
        r'appendLimit\(\s*"([^"]+)"\s*,\s*(.*?)\s*\);',
        material,
        re.DOTALL,
    ):
        limit_rows.append((name, integer_expression(expression, constants)))
    if not limit_rows:
        raise SystemExit(f"{SCRIPT_RUNTIME_SOURCE}: environment material emits no limits")

    def string_constant(name: str) -> str:
        match = re.search(
            rf"\b{name}\s*=\s*std::string_view\{{\s*\"([^\"]+)\"\s*\}}",
            runtime_header,
        )
        if match is None:
            raise SystemExit(f"{SCRIPT_RUNTIME_HEADER}: cannot read {name}")
        return match.group(1)

    globals_span = function_span(runtime_header, "k_pureGlobals")
    globals_ = re.findall(r'std::string_view\{"([^"]+)"\}', globals_span)
    api_constants = [
        ("require", "k_requireContract"),
        (
            f"resource.{string_constant('k_resourceReadJson')}",
            "k_resourceReadJsonContract",
        ),
        (
            f"resource.{string_constant('k_resourceReadText')}",
            "k_resourceReadTextContract",
        ),
        (
            f"resource.{string_constant('k_resourceReadBytes')}",
            "k_resourceReadBytesContract",
        ),
        ("tostring", "k_tostringContract"),
    ]
    contracts_start = material.index('output += ",\\"contracts\\":{";')
    contracts_end = material.index('output += "},\\"frozen_tables\\":{";', contracts_start)
    material_contracts = set(
        re.findall(
            r"appendJsonString\(output,\s*(k_\w+Contract)\)",
            material[contracts_start:contracts_end],
        )
    )
    published_contracts = {constant for _name, constant in api_constants}
    if material_contracts != published_contracts:
        raise SystemExit(
            f"{SCRIPT_RUNTIME_SOURCE}: public API contracts do not match environment material: "
            f"material={sorted(material_contracts)}, published={sorted(published_contracts)}"
        )
    apis = [(name, string_constant(constant)) for name, constant in api_constants]
    remaining = re.search(r'remaining_options\\":\\"([^"\\]+)\\"', material)
    if remaining is None:
        raise SystemExit(f"{SCRIPT_RUNTIME_SOURCE}: cannot read remaining compiler options")
    # Each row's own braces are read rather than restating its field count: a
    # PureModuleBinding row currently carries a name pair and a
    # dependencyDepth, and the lookbehind in PURE_MODULE_BINDING keeps this
    # from also matching a row of the parallel InternalPureModuleBinding
    # table, whose names are reserved and never Project-reachable. The scoped
    # table is read by the same machinery under its own pattern, because scoped
    # and pure are different contracts and must not merge into one list.
    def module_bindings(pattern: re.Pattern[str], label: str) -> list[tuple[str, str]]:
        bindings: list[tuple[str, str]] = []
        for row in pattern.finditer(framework_bundle):
            span = braced_span(framework_bundle, row.end() - 1)
            fields = [unescape(literal) for literal in STRING_LITERAL.findall(span)]
            if len(fields) < 2:
                raise SystemExit(
                    f"{FRAMEWORK_BUNDLE_SOURCE}: a {label} row holds fewer "
                    "than the private/public name pair it must declare"
                )
            private_name, public_name = fields[0], fields[1]
            if not re.fullmatch(r"[a-z0-9_-]+", private_name):
                raise SystemExit(
                    f"{FRAMEWORK_BUNDLE_SOURCE}: {label} private name "
                    f"{private_name!r} is not a bare module name"
                )
            if not re.fullmatch(r"@umbraflow/[a-z0-9_/-]+", public_name):
                raise SystemExit(
                    f"{FRAMEWORK_BUNDLE_SOURCE}: {label} public name "
                    f"{public_name!r} is not an @umbraflow/ module name"
                )
            bindings.append((private_name, public_name))
        return bindings

    def published_modules(bindings: list[tuple[str, str]]) -> list[dict[str, object]]:
        modules: list[dict[str, object]] = []
        for private_name, public_name in bindings:
            relative_path = f"{FRAMEWORK_RUNTIME_DIRECTORY}/{private_name}.luau"
            module_source = read(root, relative_path)
            target = re.escape(private_name)
            exports = sorted(
                set(
                    re.findall(
                        rf"^function\s+{target}\.([a-z][a-z0-9_]*)\s*\(",
                        module_source,
                        re.MULTILINE,
                    )
                    + re.findall(
                        rf"^{target}\.([a-z][a-z0-9_]*)\s*=",
                        module_source,
                        re.MULTILINE,
                    )
                )
            )
            if not exports:
                raise SystemExit(f"{relative_path}: publishes no readable exports")
            modules.append(
                {
                    "name": public_name,
                    "exports": exports,
                    "source_hash": hashlib.sha256(
                        (root / relative_path).read_bytes()
                    ).hexdigest(),
                }
            )
        modules.sort(key=lambda module: str(module["name"]))
        return modules

    pure_bindings = module_bindings(PURE_MODULE_BINDING, "PureModuleBinding")
    if not pure_bindings:
        raise SystemExit(
            f"{FRAMEWORK_BUNDLE_SOURCE}: exposes no reserved Framework modules"
        )
    scoped_bindings = module_bindings(SCOPED_MODULE_BINDING, "ScopedModuleBinding")
    if not scoped_bindings:
        raise SystemExit(
            f"{FRAMEWORK_BUNDLE_SOURCE}: exposes no scoped Framework modules"
        )
    # The two contracts must not overlap: one name that appeared in both tables
    # would be a module a reader could not tell which program type loads it.
    shared = {name for _stem, name in pure_bindings} & {
        name for _stem, name in scoped_bindings
    }
    if shared:
        raise SystemExit(
            f"{FRAMEWORK_BUNDLE_SOURCE}: {sorted(shared)} are declared both pure "
            "and scoped, so the published contract cannot say which they are"
        )
    reserved_modules = published_modules(pure_bindings)
    scoped_modules = published_modules(scoped_bindings)
    return {
        "apis": apis,
        "globals": globals_,
        "reserved_modules": reserved_modules,
        "scoped_modules": scoped_modules,
        "limits": limit_rows,
        "module_grammar_contract": string_constant("k_moduleGrammarContract"),
        "resource_grammar_contract": string_constant("k_resourceGrammarContract"),
        "failure_contract": string_constant("k_moduleFailureContract"),
        "interrupt_contract": string_constant("k_interruptContract"),
        "luau": string_constant("k_luauImplementation"),
        "compiler": {
            "optimization_level": constants["k_compileOptimizationLevel"],
            "debug_level": constants["k_compileDebugLevel"],
            "remaining_options": remaining.group(1),
        },
    }


def render(root: Path) -> str:
    published = published_schemas(root)
    embedded = embedded_schema_documents(root)
    targets = reference_targets(root)
    tags = written_wire_tags(root)
    pinned_tags = {identity.wire_tag for identity in published if identity.wire_tag}
    release_facts = release_manifest_facts(root)
    directory_schema = json.loads(read(root, PROJECT_DIRECTORY_SCHEMA))
    deployment_definition = definition(directory_schema, "Deployment")
    closure_definition = definition(directory_schema, "Closure")
    module_definition = definition(directory_schema, "Module")
    resource_definition = definition(directory_schema, "Resource")
    tool_definition = definition(directory_schema, "Tool")
    module_name_definition = definition(directory_schema, "ModuleName")
    resource_kind_definition = definition(directory_schema, "ResourceKind")
    script_contract = script_runtime_contract(root)
    authorities = deployment_authorities(root)
    tool_maximum = tool_declaration_bound(root)
    project_tool_body = tool_definition["properties"]["body"].get("const")
    if project_tool_body is not False:
        raise SystemExit(
            f"{PROJECT_DIRECTORY_SCHEMA}: Project Tool body must be pinned false"
        )
    proposal = json.loads(read(root, OBSERVATION_PROPOSAL_SCHEMA))
    observation = json.loads(read(root, OBSERVATION_SCHEMA))
    authority_tag, authority_members = authority_input_members(root)
    conventions = exit_code_conventions(root)
    projects = fixture_projects(root)
    refusals = refusal_sources(root)

    lines: list[str] = [
        f"<!-- Generated by {GENERATOR_RELATIVE_PATH}. Do not edit by hand; "
        f"re-run the generator. -->",
        "",
        "# Public contract",
        "",
        "The one document this repository publishes outward. Every fact below is",
        "derived from bytes in this repository by",
        f"`{GENERATOR_RELATIVE_PATH}`, so a consuming repository reads this and",
        "nothing else of ours -- no plan, no review, no archive.",
        "",
        "It states what this repository offers. It states no status, no gate, no",
        "commit and no version of any consumer: those are facts about progress, and",
        "a consumer that transcribes them transcribes something it cannot verify.",
        "",
        "## 1. Schema identities",
        "",
        "### 1.1 Published schemas",
        "",
        "Documents this repository publishes as exact bytes under a stable `$id`.",
        "Parity for one of these is byte identity: read the file, do not copy it.",
        "",
    ]
    lines.extend(
        table(
            ["`$id`", "Version", "Wire tag", "Bytes", "Required members"],
            [
                [
                    f"`{identity.schema_id}`",
                    identity.version,
                    f"`{identity.wire_tag}`" if identity.wire_tag else "--",
                    f"`{identity.origin}`",
                    cell(identity.shape),
                ]
                for identity in published
            ],
        )
    )
    embedded_rows: list[list[str]] = []
    unclassified: list[tuple[str, str]] = []
    labels = compile_labels(root)
    for schema_id, relative, document in embedded:
        tag = sorted(set(PINNED_WIRE_TAG.findall(json.dumps(document))))
        if tag:
            ownership = "wire_tag_owned"
        elif schema_id in targets:
            ownership = "embedded_fragment"
        elif schema_id.startswith("https://umbraflow.dev/schema/operator/"):
            ownership = "operator_protocol"
        else:
            ownership = "unclassified"
            label = next(
                (
                    known
                    for known in labels
                    if schema_id.endswith(f"/{known}") or schema_id.endswith(known)
                ),
                "",
            )
            unclassified.append((schema_id, label))
        embedded_rows.append(
            [
                f"`{schema_id}`",
                ownership,
                f"`{tag[0]}`" if tag else "--",
                f"`{relative}`",
                cell(shape_of(document)),
            ]
        )

    lines.extend(
        [
            "",
            "### 1.2 Identities compiled from module bytes",
            "",
            "Schemas this repository compiles out of its own source rather than",
            "publishing as a file. A project's plugin answers the operator-protocol",
            "identities and writes the documents the tag-bearing ones govern, so both",
            "are part of the surface even though there is no file to read.",
            "",
            "`wire_tag_owned` means the shape is fixed by the exact wire tag its",
            "`schema` member must carry. `embedded_fragment` means the identity is",
            "reached only as a `$ref` target of another owned schema, so ownership is",
            "transitive. `operator_protocol` means the source compiles an identity",
            "inside the framework-owned `operator/` namespace for the ProjectPlugin",
            "call boundary. `unclassified` means none is true of it in these bytes.",
            "",
        ]
    )
    lines.extend(
        table(
            ["`$id`", "Ownership", "Wire tag", "Source", "Required members"],
            embedded_rows,
        )
    )

    unpinned = sorted(tags - pinned_tags)
    lines.extend(
        [
            "",
            "### 1.3 Wire tags no published schema pins",
            "",
            "Tags this repository's sources write and read, for which no file under",
            f"`{SCHEMA_DIRECTORY}/` pins the value. The tag is the whole ownership",
            "statement.",
            "",
        ]
    )
    lines.extend(
        table(
            ["Wire tag"],
            [[f"`{tag}`"] for tag in unpinned],
        )
    )

    release_member_meanings = {
        "schema": "the manifest's own wire tag",
        "release": "milestone name, e.g. `m0-acceptance`",
        "contract_versions": "the format versions this release's tooling understands",
        "artifacts": "one row per shipped binary or runtime payload file",
    }
    artifact_member_meanings = {
        "name": "logical binary name",
        "platform": "`windows`, `linux` or `macos`",
        "arch": "`x64` or `arm64`",
        "path": "canonical `'/'`-only path relative to the release root",
        "asset": "the flat asset name a GitHub release carries it under",
        "sha256": "lowercase hex content digest, no prefix",
    }
    lines.extend(
        [
            "",
            "### 1.4 The Project Kit release manifest",
            "",
            f"A release bundle ships an immutable manifest tagged `{release_facts['tag']}`,",
            f"written by `{RELEASE_SOURCE}` and never authored by hand.",
            "`project upgrade` parses it, selects the artifact for the host",
            "platform and arch, and refuses a mismatch on the declared sha256. The",
            "release id is the sha256 of the manifest's canonical bytes, derived",
            "rather than stored.",
            "",
        ]
    )
    for headings, members, meanings in (
        (
            ["Top-level member", "Meaning"],
            release_facts["members"],
            release_member_meanings,
        ),
        (
            ["Artifact-row member", "Meaning"],
            release_facts["artifact_members"],
            artifact_member_meanings,
        ),
    ):
        unknown = [member for member in members if member not in meanings]
        if unknown:
            raise SystemExit(
                f"{RELEASE_SOURCE} gained members with no meaning in "
                f"{GENERATOR_RELATIVE_PATH}: {unknown}"
            )
        lines.extend(
            table(
                headings,
                [[f"`{member}`", meanings[member]] for member in members],
            )
        )
        lines.append("")
    lines.extend(
        [
            "`contract_versions` carries "
            + ", ".join(
                f"`{value}`" for value in release_facts["contract_versions"]
            )
            + "; the shipped binaries are "
            + ", ".join(f"`{value}`" for value in release_facts["binaries"])
            + ". The release also carries the runtime payload "
            + ", ".join(
                f"`{value}`" for value in release_facts["payload_patterns"]
            )
            + ", each matched file one artifact row whose path `project upgrade` "
            + "restores beside the binaries.",
            "",
        ]
    )

    lines.extend(
        [
            "",
            "## 2. What a consumer must declare",
            "",
            "A project is a directory. It holds",
            f"`{directory_schema.get('title')}` at its root, and that document is",
            "judged against the published schema",
            f"`{directory_schema.get('$id')}`.",
            "",
            "### 2.1 The root document",
            "",
        ]
    )
    lines.extend(
        table(
            ["Required member"],
            [
                [f"`{member}`"]
                for member in sorted(directory_schema.get("required", []))
            ],
        )
    )
    lines.extend(
        [
            "",
            "### 2.2 Each deployment block",
            "",
            "A deployment block **is** its registration, stated as intent: it names",
            "its closure and its resources by path, states everything else inline,",
            "and the loader derives every registration digest from the bytes it",
            "read. No member below is a hash.",
            "",
        ]
    )
    lines.extend(
        table(
            ["Required member"],
            [
                [f"`{member}`"]
                for member in sorted(deployment_definition.get("required", []))
            ],
        )
    )
    lines.extend(
        [
            "",
            "### 2.3 Module and resource closures",
            "",
            "A deployment states ONE closure, `tool_closure`, compiled on the",
            "scoped Tool program type. A project that binds no Tool ships that",
            "closure with an explicitly empty `exported_entry_points` and an",
            "empty `tool_bindings`; there is no absent form of either.",
            "",
            "The closure is an explicit closed module graph. `entry` selects one",
            "logical module name, `exported_entry_points` STATES what that graph",
            "exports and is never derived from the binding table, and every module",
            "and resource path is confined to the project directory, while runtime",
            "identity retains names, kinds, sizes and exact byte hashes but no host",
            "path. Authored array order is not identity.",
            "",
            "Closure required members: "
            + ", ".join(f"`{value}`" for value in closure_definition["required"])
            + ". Module required members: "
            + ", ".join(f"`{value}`" for value in module_definition["required"])
            + ". Resource required members: "
            + ", ".join(f"`{value}`" for value in resource_definition["required"])
            + ".",
            "",
            f"Module-name grammar: `{module_name_definition['pattern']}`; resource",
            "names use "
            f"`{resource_definition['properties']['name']['pattern']}`. Resource",
            "kinds are "
            + ", ".join(f"`{value}`" for value in resource_kind_definition["enum"])
            + ".",
            "",
            "### 2.4 The authorities each deployment yields",
            "",
            "Loading one deployment block builds these, and a directory is accepted",
            "only when every one of them compiles. Read from",
            f"`{DEPLOYMENT_DIRECTORY_HEADER}`.",
            "",
        ]
    )
    lines.extend(
        table(
            ["Authority", "Member"],
            [[f"`{type_name}`", f"`{member}`"] for type_name, member in authorities],
        )
    )
    lines.extend(
        [
            "",
            "### 2.5 The Tool declarations",
            "",
            "A deployment declares its Tools inline, in `tools`. There is no Tool",
            "Catalog document and no catalog file: these entries are the whole",
            f"declaration, at most {tool_maximum} of them, and their exact canonical",
            "bytes are what `tool_catalog_hash` digests. The array may be empty --",
            "that is a project declaring no Tool, stated rather than omitted. A tool",
            "declares its own `mutability`, so a project that publishes no *mutating*",
            "tool is expressible: every tool declares `read_only`.",
            "",
            "Every Tool member below is mandatory:",
            "",
            *table(
                ["Required member"],
                [
                    [f"`{member}`"]
                    for member in sorted(tool_definition["required"])
                ],
            ),
            "",
            "A Project Tool declares `body: false`: Project entry points take only",
            "their arguments and cannot receive a caller-supplied body. Framework",
            "built-ins declare their own body shape separately.",
            "",
            "Each entry carries `argument_schema` as a mandatory member whose value",
            "is either the string `unchecked` or an inline JSON Schema object. The",
            "two mean different things: `unchecked` is the project declining",
            "argument validation, and the framework still records the exact bytes it",
            "passed, their digest and their coordinates.",
            "",
            "## 3. CLI surface",
            "",
            "### 3.1 Invocation",
            "",
        ]
    )
    for relative, name in CLI_USAGE_DEFINITIONS:
        usage = definition_literal(read(root, relative), name)
        lines.append("```")
        lines.extend(usage_lines(usage))
        lines.append("```")
        lines.append("")

    lines.extend(
        [
            "### 3.2 Exit codes",
            "",
        ]
    )
    for binary, codes in conventions:
        lines.append(f"`{binary}`:")
        lines.append("")
        lines.extend(
            table(
                ["Code", "Meaning"],
                [[value, f"`{name}`"] for name, value in codes],
            )
        )
        lines.append("")

    lines.extend(
        [
            "### 3.3 Refusals",
            "",
            "The exact text a refused project directory produces, read out of the",
            "sources that print it. A `{}` is a value the message interpolates.",
            "",
        ]
    )
    for source in refusals:
        lines.append(f"#### {source.label}")
        lines.append("")
        lines.append(f"`{source.path}`")
        lines.append("")
        lines.append("```text")
        lines.extend(source.messages)
        lines.append("```")
        lines.append("")

    compiler = script_contract["compiler"]
    lines.extend(
        [
            "## 4. Luau execution environment",
            "",
            "Project code executes as a closed, pathless module graph in a fresh",
            "quota-bound VM. `require` resolves only the registration-pinned module",
            "closure; it has no filesystem, network, package search path or ambient",
            "asset authority. Resources are the separately pinned read-only data",
            "closure.",
            "",
            "### 4.1 Published APIs and observable contracts",
            "",
        ]
    )
    lines.extend(
        table(
            ["API", "Environment contract"],
            [[f"`{name}`", f"`{contract}`"] for name, contract in script_contract["apis"]],
        )
    )
    lines.extend(
        [
            "",
            "`resource.readJson` returns one deeply frozen decoded JSON identity per",
            "VM; `resource.readText` returns admitted UTF-8; `resource.readBytes`",
            "returns exact bytes. All require exactly one canonical resource name and",
            "refuse unknown names or kind mismatches.",
            "",
            "Published globals: "
            + ", ".join(f"`{name}`" for name in script_contract["globals"])
            + ".",
            "",
            "Reserved pure Framework modules: "
            + ", ".join(
                f"`{module['name']}`"
                for module in script_contract["reserved_modules"]
            )
            + ".",
            "Their exact source bytes are release-owned, identity-bound,",
            "deeply frozen after loading and cannot import the Project graph.",
            "Unicode algorithms load release-owned data through reserved internal",
            "Framework modules that Project-authored modules cannot resolve.",
            f"Unicode-derived data is distributed under `{UNICODE_LICENSE}`.",
            "",
        ]
    )
    lines.extend(
        table(
            ["Reserved module", "Exports", "Source SHA-256"],
            [
                [
                    f"`{module['name']}`",
                    ", ".join(f"`{name}`" for name in module["exports"]),
                    f"`{module['source_hash']}`",
                ]
                for module in script_contract["reserved_modules"]
            ],
        )
    )
    lines.extend(
        [
            "",
            "Reserved SCOPED Framework modules: "
            + ", ".join(
                f"`{module['name']}`"
                for module in script_contract["scoped_modules"]
            )
            + ".",
            "These are a DIFFERENT contract from the pure modules above and are",
            "not interchangeable with them. A pure module loads in every Project",
            "program of the pure type. A scoped module loads only",
            "inside a scoped Tool execution",
            "program, and `require` of one of these names from any other program",
            "fails in the resolver naming the module, because the scoped set is a",
            "property of the program type rather than of a runtime flag. They are",
            "absent from the pure module closure, from the trusted framework",
            "bundle, and from every project-global projection.",
            "",
            "A scoped module reaches the world only by making an ordinary Tool",
            "call through one private capability primitive it is handed as its",
            "chunk argument and cannot republish. Every such call creates a",
            "ToolInvocation, spends Tool-call budget, and is recorded at a call",
            "position the host assigns; a script cannot name, pass, or influence",
            "that position. Tool discovery and description are frozen data read",
            "from the run's pinned Tool catalog resource, not a call. A Tool",
            "Runtime refusal is terminal for the run and no `pcall` can observe",
            "it, while a Tool that ran and failed reports its delivery",
            "classification inside the value it answers with.",
            "",
        ]
    )
    lines.extend(
        table(
            ["Reserved scoped module", "Exports", "Source SHA-256"],
            [
                [
                    f"`{module['name']}`",
                    ", ".join(f"`{name}`" for name in module["exports"]),
                    f"`{module['source_hash']}`",
                ]
                for module in script_contract["scoped_modules"]
            ],
        )
    )
    lines.extend(
        [
            "",
            "### 4.2 Identity preimage",
            "",
            f"`plugin_environment_hash` is SHA-256 over exact canonical bytes emitted",
            f"by `currentProjectPluginEnvironmentMaterial()` in `{PROJECT_PLUGIN_SOURCE}`.",
            "The preimage contains the pure-data environment material; every public",
            "and internal Framework module name, source hash, dependency depth, and",
            "Project visibility; and the freeze and separate release-owned budget",
            "contracts. The nested pure-data material contains the trusted bridge",
            "source; compiler options; API contracts; frozen tables and global",
            "whitelist; grammar, interrupt and module-failure contracts; the module",
            "resolver's reserved prefix and caller-relative markers; every numeric",
            "limit below; and the pinned Luau implementation.",
            "",
            f"Compiler: optimization `{compiler['optimization_level']}`, debug",
            f"`{compiler['debug_level']}`, remaining options",
            f"`{compiler['remaining_options']}`. Luau: `{script_contract['luau']}`.",
            "",
            f"Module grammar contract: `{script_contract['module_grammar_contract']}`;",
            f"resource grammar contract: `{script_contract['resource_grammar_contract']}`;",
            f"module failure contract: `{script_contract['failure_contract']}`;",
            f"interrupt contract: `{script_contract['interrupt_contract']}`.",
            "",
            "### 4.3 Enforced limits",
            "",
        ]
    )
    lines.extend(
        table(
            ["Environment-material member", "Value"],
            [[f"`{name}`", f"`{value}`"] for name, value in script_contract["limits"]],
        )
    )
    lines.append("")

    call_states = tool_call_states(root)
    envelope = tool_answer_envelope(root)
    root_identity = tool_root_identity(root)
    protocol_members = tool_protocol_material(root)
    ledger_text = read(root, LEDGER_SOURCE)
    invocation_text = read(root, TOOL_INVOCATION_SOURCE)
    deployment_text = read(root, DEPLOYMENT_PROTOCOL_SOURCE)
    protocol_fragments = sorted(set(re.findall(
        r"operator_runtime::(\w+Material)\(\)",
        function_span(deployment_text, "currentToolRuntimeProtocolMaterial"),
    )))
    if not protocol_fragments:
        raise SystemExit(
            f"{DEPLOYMENT_PROTOCOL_SOURCE}: the protocol material assembles no"
            " module-owned fragment"
        )
    catalog_mismatch_refusal = one_message_in(
        invocation_text,
        "ProjectToolCatalogSchemaOwner::create",
        "tool_catalog_hash",
    )

    if envelope["states"] != [name for name, _carries in call_states]:
        raise SystemExit(
            f"{TOOL_RUNTIME_LUAU}: the delivery vocabulary a script reads is not "
            f"the Operator's: script={envelope['states']}, "
            f"operator={[name for name, _carries in call_states]}"
        )

    lines.extend(
        [
            "## 5. The Tool Runtime",
            "",
            "A Tool call is the only way project code reaches the world, and the",
            "only way a caller outside this repository drives one. Everything in",
            "this section is read out of the sources that decide it.",
            "",
            "### 5.1 Call states",
            "",
            "The durable vocabulary a call's row may carry. *Carries an outcome* is",
            "the terminal split: a state carries one exactly when a provider",
            "conclusion or a reconciliation wrote it, and the three that do not are",
            "the three a call is still passing through. `possible` and",
            "`terminally_unresolved` carry an outcome and still hold the",
            "target-wide mutation barrier, because neither says what the world did.",
            "",
            f"Read from `k_toolCallStateNames` and `toolCallStateHasOutcome` in",
            f"`{TOOL_RUNTIME_SOURCE}`. A scoped script reads the same set as",
            f"`tools.states` from `{TOOL_RUNTIME_LUAU}`, and this generator",
            "requires the two to be the same list in the same order.",
            "",
        ]
    )
    lines.extend(
        table(
            ["State", "Carries an outcome"],
            [
                [f"`{name}`", "yes" if carries else "no"]
                for name, carries in call_states
            ],
        )
    )
    lines.extend(
        [
            "",
            "### 5.2 The answer a script sees",
            "",
            f"One Tool call answers one frozen object. `checkedAnswer` in",
            f"`{TOOL_RUNTIME_LUAU}` rejects anything else, so these member names",
            "are the whole of what an answer is:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Answer member"],
            [[f"`{member}`"] for member in envelope["members"]],
        )
    )
    lines.extend(
        [
            "",
            "`result` and `evidence` are absent when the recorded outcome carries",
            "none, and their accessors answer a `@umbraflow/result` failure rather",
            "than a value -- "
            + ", ".join(f"`{code}`" for code in envelope["absent_result_codes"])
            + ".",
            "",
            "A Tool Runtime *refusal* never reaches this envelope at all: it is",
            "terminal for the run, the VM is destroyed without resuming the script,",
            "and no `pcall` can observe one. A Tool that ran and failed is not a",
            "refusal -- its classification travels in `state`.",
            "",
            "### 5.3 Root request idempotency",
            "",
            "A run is opened by a root request, and a root request is identified by",
            f"SHA-256 over a preimage tagged `{root_identity['root_tag']}` carrying",
            "exactly "
            + ", ".join(f"`{part}`" for part in root_identity["root_parts"])
            + ". A positioned call inside that run is identified by SHA-256 over a",
            f"preimage tagged `{root_identity['call_tag']}`.",
            "",
            "Two root requests therefore stand in exactly one of three relations,",
            f"decided by `ToolRootRequestIdentity::relationTo` in",
            f"`{TOOL_INVOCATION_SOURCE}`:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Relation", "When"],
            [
                [
                    "`SameRequest`",
                    "same caller namespace and request key, and byte-identical "
                    "request preimage -- the second call rejoins the first",
                ],
                [
                    "`Conflict`",
                    "same caller namespace and request key, different request "
                    "preimage bytes -- refused rather than opening a second run",
                ],
                [
                    "`Distinct`",
                    "a different caller namespace or a different request key",
                ],
            ],
        )
    )
    if sorted(root_identity["relations"]) != ["Conflict", "Distinct", "SameRequest"]:
        raise SystemExit(
            f"{TOOL_INVOCATION_SOURCE}: relationTo no longer decides exactly the "
            f"three relations published here: {sorted(root_identity['relations'])}"
        )
    lines.extend(
        [
            "",
            "### 5.4 `tool_catalog_hash`",
            "",
            "A project's `tool_catalog_hash` is the SHA-256 of the exact canonical",
            "(RFC 8785 JCS) bytes of the `tools` array its deployment declared.",
            f"There is no Tool Catalog document: `{PROJECT_DIRECTORY_SCHEMA}` states",
            "the shape of those entries and the loader renders them canonically.",
            "Nothing derives the digest from a parse:",
            "`ProjectToolCatalogSchemaOwner::create` in",
            f"`{TOOL_INVOCATION_SOURCE}` hashes the supplied bytes and refuses the",
            "deployment when the digest is not the one the registration pinned:",
            "",
            "```text",
            catalog_mismatch_refusal,
            "```",
            "",
            "`tool_catalog_hash` is a member of the canonical registration, so it",
            "reaches `project_registration_hash`; a call minted against other",
            "declaration bytes cannot present this registration.",
            "",
            "### 5.5 The durable record",
            "",
            "What one Tool call leaves behind, and which of it is reproducible. A",
            "reproducible row is a function of the request and the release, so a",
            "replaying incarnation derives the identical row; an admission attempt",
            "is not, and is published as such rather than omitted.",
            "",
            "**Reproducible.** `tool_root_requests` -- the run the caller asked for:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Column"],
            [
                [f"`{column}`"]
                for column in ddl_columns(ledger_text, "k_toolRootRequestsDdl")
            ],
        )
    )
    lines.extend(
        [
            "",
            "**Reproducible.** `tool_call_positions` -- where in the run the call is,",
            "and the exact material its identity is taken over:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Column"],
            [
                [f"`{column}`"]
                for column in ddl_columns(ledger_text, "k_toolCallPositionsDdl")
            ],
        )
    )
    lines.extend(
        [
            "",
            "**Deliberately not reproducible.** `tool_admission_attempts` records the",
            "live authority one attempt was admitted against -- the lease, the",
            "fencing token, the session epoch, the budget as it stood, the policy",
            "and capability digests. None of that is a function of the request: a",
            "second attempt at the identical call is admitted against whatever",
            "authority is live then, and it is a *new* row rather than the same one",
            "recomputed. A consumer that re-derived these would be inventing an",
            "authority nobody held.",
            "",
        ]
    )
    lines.extend(
        table(
            ["Column"],
            [
                [f"`{column}`"]
                for column in ddl_columns(ledger_text, "k_toolAdmissionAttemptsDdl")
            ],
        )
    )
    lines.extend(
        [
            "",
            "### 5.6 `tool_runtime_protocol_identity`",
            "",
            "`tool_runtime_protocol_identity` is the SHA-256 of the exact canonical",
            f"bytes emitted by `currentToolRuntimeProtocolMaterial()` in",
            f"`{DEPLOYMENT_PROTOCOL_SOURCE}`. The recording incarnation writes it",
            "into the `tool_runs` row; a resuming or replaying incarnation derives",
            "it again from its own release bytes and refuses the continuation when",
            "the two differ. It is an assertion inside the resuming reader and is",
            "never a dispatch key.",
            "",
            "The preimage carries exactly these members:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Preimage member"],
            [[f"`{member}`"] for member in protocol_members],
        )
    )
    lines.extend(
        [
            "",
            "Each is rendered by the module that owns what it describes: "
            + ", ".join(f"`{name}()`" for name in protocol_fragments)
            + ".",
            "",
            "It is deliberately not `tool_catalog_hash`, which covers Framework Tool",
            "descriptors and nothing else: the state vocabulary, the identity",
            "preimages, the durable record and the canonical-form contract can every",
            "one of them change without moving that digest.",
            "",
        ]
    )

    proposal_required = item_requirements(proposal, "observed_instance_proposals")
    observation_required = item_requirements(observation, "observed_instances")
    minted_pattern = item_pattern(
        observation, "observed_instances", "observed_instance_id"
    )
    lines.extend(
        [
            "## 6. The ownership boundary",
            "",
            "An observed instance identity is the sharpest edge of this boundary. The",
            "project states what a thing *is*; the Operator decides which thing it is",
            "and names it. The project never mints an id, a hash, or canonical",
            "identity bytes.",
            "",
            "### 6.1 What the project supplies",
            "",
            f"One entry of `observed_instance_proposals` in `{proposal.get('$id')}`:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Required member"],
            [[f"`{member}`"] for member in proposal_required],
        )
    )
    lines.extend(
        [
            "",
            "### 6.2 What the Operator does with it",
            "",
            "The Operator validates the proposal, canonicalizes it (RFC 8785 JCS),",
            "binds it to a scope, and mints the id. The canonical authority input it",
            f"binds is tagged `{authority_tag}` and carries exactly these members:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Authority-input member"],
            [[f"`{member}`"] for member in authority_members],
        )
    )
    lines.extend(
        [
            "",
            "### 6.3 What comes back",
            "",
            f"One entry of `observed_instances` in `{observation.get('$id')}`:",
            "",
        ]
    )
    lines.extend(
        table(
            ["Required member"],
            [[f"`{member}`"] for member in observation_required],
        )
    )
    lines.extend(
        [
            "",
            f"`observed_instance_id` is opaque and matches `{cell(minted_pattern)}`.",
            "The project reads it and passes it back; it never derives one.",
            "",
            "## 7. Worked examples",
            "",
            "Two fixture project directories in this repository, each written the way",
            "a consuming repository writes its own, and each run by this repository's",
            "own CI under the CTest name below. They are the documents to copy.",
            "",
        ]
    )
    for test_name, directory, manifest in projects:
        lines.append(f"### `{directory}`")
        lines.append("")
        lines.append(f"CTest name: `{test_name}`")
        lines.append("")
        lines.extend(
            table(
                ["Deployment", "`plugin_id`", "`plugin_authoring`", "Primary"],
                [
                    [
                        f"`{deployment.get('name')}`",
                        f"`{deployment.get('plugin_id')}`",
                        f"`{deployment.get('plugin_authoring')}`",
                        "yes"
                        if deployment.get("name") == manifest.get("primary_deployment")
                        else "no",
                    ]
                    for deployment in manifest.get("deployments", [])
                ],
            )
        )
        lines.append("")

    if unclassified:
        lines.extend(
            [
                "## 8. Identities this generator cannot classify",
                "",
                "Each identity below is compiled from module bytes, pins no wire tag,",
                "and is no schema's `$ref` target, so none of the ownership",
                "categories follows from these bytes. Listed rather than guessed: the",
                "compile label is the only byte that ties one to a caller.",
                "",
            ]
        )
        lines.extend(
            table(
                ["`$id`", "Compile label"],
                [
                    [f"`{schema_id}`", f"`{label}`" if label else "--"]
                    for schema_id, label in unclassified
                ],
            )
        )
        lines.append("")

    return "\n".join(line.rstrip() for line in lines).rstrip("\n") + "\n"


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    argument_parser.add_argument(
        "--check",
        action="store_true",
        help="fail when the committed document differs from a regeneration",
    )
    arguments = argument_parser.parse_args()

    root = arguments.root.resolve()
    output = root / OUTPUT_RELATIVE_PATH
    generated = render(root)

    if not arguments.check:
        output.write_text(generated, encoding="utf-8", newline="\n")
        print(f"Public contract generated ({len(generated)} bytes): {OUTPUT_RELATIVE_PATH}")
        return 0

    violations: list[str] = []
    if not output.is_file():
        violations.append(f"{OUTPUT_RELATIVE_PATH}: is not committed")
    elif output.read_text(encoding="utf-8") != generated:
        violations.append(
            f"{OUTPUT_RELATIVE_PATH}: differs from what "
            f"{GENERATOR_RELATIVE_PATH} generates from this tree"
        )

    if violations:
        print("Public contract violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        print(
            f"Run python {GENERATOR_RELATIVE_PATH} to regenerate it; never edit it "
            "by hand.",
            file=sys.stderr,
        )
        return 1

    print(f"Public contract check OK ({len(generated)} bytes).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
