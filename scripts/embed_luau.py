#!/usr/bin/env python3
"""Embed a module's .luau sources into one generated C++ translation unit.

The generated file is written into the build tree and is never committed, so
the .luau files under the module remain the single source of truth for the
trusted framework bundle (docs/archive/plans/2026-07-29-three-layer-task-system.md 14).

Determinism is a hard requirement: the same inputs always produce a
byte-identical output file.  Sources are ordered by their POSIX-normalized
relative path, hashes are plain SHA-256, and nothing about the host (path
separators, locale, environment, wall clock) reaches the output.

Encoding choice -- escaped string literals, not raw string literals.  A raw
literal with a fixed delimiter is unsafe twice over: a .luau file may contain
the delimiter, and [lex.pptoken] mandates that a CRLF pair inside a raw literal
is translated to a single LF.  Either one silently changes the bytes the
recorded hash certifies.  Escaping instead -- printable ASCII verbatim, the two
common whitespace bytes as \\n and \\t, every remaining byte as a three-digit
octal escape -- cannot be broken by the file's own content and reproduces the
source bytes exactly, while keeping the generated file readable and greppable.
Octal rather than hex because an octal escape consumes exactly three digits, so
a following literal digit can never be absorbed into it.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import sys
from dataclasses import dataclass
from pathlib import Path


# The module name a .luau file contributes to the bundle is its stem, so the
# stem must be a usable identifier-like key rather than an arbitrary filename.
MODULE_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*$")

# MSVC caps one string literal at 16383 bytes (C2026); adjacent-literal
# concatenation does not count toward it.  Both measured on cl 19.44, which is
# why a .luau file of any size embeds and why no per-file cap belongs here.
MAXIMUM_CHUNK_BYTES = 2000

# Separator between a module name and its bytes in the bundle-hash preimage.
# NUL cannot occur in either, so the concatenation is unambiguous.
NAME_SEPARATOR = b"\x00"


@dataclass(frozen=True)
class LuauSource:
    name: str
    relative_path: str
    data: bytes

    @property
    def digest(self) -> str:
        return hashlib.sha256(self.data).hexdigest()


class EmbedError(Exception):
    """A .luau source violates the contract the embedder can honour."""


def validate_source(relative_path: str, data: bytes) -> None:
    """Reject every input the embedding or the hash could not carry faithfully.

    Failing here is deliberate: each of these would otherwise surface either as
    an opaque compiler diagnostic inside a generated file, or -- worse -- as an
    embedded byte string that silently differs from the file the hash covers.
    """
    try:
        data.decode("utf-8")
    except UnicodeDecodeError as error:
        raise EmbedError(f"{relative_path}: not valid UTF-8 ({error})") from error

    if b"\x00" in data:
        raise EmbedError(f"{relative_path}: contains a NUL byte")

    if b"\r" in data:
        raise EmbedError(
            f"{relative_path}: contains a CR byte; the repository line-ending "
            "policy is LF only and CR would not survive embedding faithfully"
        )


def collect_sources(source_dir: Path) -> list[LuauSource]:
    paths = sorted(source_dir.rglob("*.luau"), key=lambda path: path.as_posix())
    sources: list[LuauSource] = []
    seen: dict[str, str] = {}

    for path in paths:
        relative_path = path.relative_to(source_dir).as_posix()
        name = path.stem

        if not MODULE_NAME.match(name):
            raise EmbedError(
                f"{relative_path}: module name {name!r} must match {MODULE_NAME.pattern}"
            )

        previous = seen.get(name)
        if previous is not None:
            raise EmbedError(
                f"{relative_path}: module name {name!r} already taken by {previous}"
            )
        seen[name] = relative_path

        data = path.read_bytes()
        validate_source(relative_path, data)
        sources.append(LuauSource(name=name, relative_path=relative_path, data=data))

    return sources


def bundle_digest(sources: list[LuauSource]) -> str:
    """SHA-256 over name || 0x00 || bytes for every source, in bundle order."""
    accumulator = hashlib.sha256()
    for source in sources:
        accumulator.update(source.name.encode("utf-8"))
        accumulator.update(NAME_SEPARATOR)
        accumulator.update(source.data)
    return accumulator.hexdigest()


def escape_byte(value: int) -> str:
    if value == 0x22:
        return '\\"'
    if value == 0x5C:
        return "\\\\"
    if value == 0x0A:
        return "\\n"
    if value == 0x09:
        return "\\t"
    if 0x20 <= value <= 0x7E:
        return chr(value)
    # A three-digit octal escape consumes exactly three digits, so a following
    # literal digit can never be absorbed into it the way it can with \x.
    return f"\\{value:03o}"


def chunk(data: bytes) -> list[bytes]:
    """Split into per-line pieces, further split so no piece is oversized."""
    pieces: list[bytes] = []
    for line in data.splitlines(keepends=True):
        for offset in range(0, len(line), MAXIMUM_CHUNK_BYTES):
            pieces.append(line[offset : offset + MAXIMUM_CHUNK_BYTES])
    return pieces


def render_source_literal(source: LuauSource, indent: str) -> str:
    pieces = chunk(source.data)
    if not pieces:
        return f'{indent}""'
    return "\n".join(
        f'{indent}"{"".join(escape_byte(value) for value in piece)}"' for piece in pieces
    )


def render(sources: list[LuauSource], version: str, label: str) -> str:
    lines: list[str] = []
    append = lines.append

    append("// GENERATED FILE -- DO NOT EDIT AND DO NOT COMMIT.")
    append("//")
    append(f"// Produced by scripts/embed_luau.py from {label}.")
    append("// The CMake module autoloader regenerates it whenever a .luau source under")
    append("// that directory is added, edited, or deleted, so the .luau files stay the")
    append("// single source of truth for the bundle.")
    append("//")
    append("// Encoding: every byte is emitted as an escaped C++ string literal -- printable")
    append("// ASCII verbatim, LF and TAB as \\n and \\t, every remaining byte as a three-digit")
    append("// octal escape -- chunked into adjacent literals that the compiler concatenates,")
    append("// so each literal below holds exactly the bytes on disk. Raw string literals are")
    append("// deliberately NOT used: a fixed raw delimiter can occur inside a .luau file,")
    append("// and the standard mandates translating a CRLF pair inside a raw literal to a")
    append("// single LF. Either would silently change the bytes the recorded hash certifies.")
    append("//")
    append("// Bundle hash recipe, recomputable by hand: concatenate, for each entry below in")
    append("// order, the module name in UTF-8, one 0x00 separator byte, then the exact bytes")
    append("// of that module's .luau file; the bundle hash is the SHA-256 of that byte")
    append("// string, in lowercase hex.")
    append("")
    append("#include <task/framework-bundle.hpp>")
    append("")
    append("#include <array>")
    append("#include <span>")
    append("#include <string_view>")
    append("")
    append("namespace uf::task")
    append("{")
    append("    namespace")
    append("    {")
    append(f'        constexpr auto k_frameworkVersion = std::string_view{{"{version}"}};')
    append(
        f'        constexpr auto k_bundleHash       = std::string_view{{"{bundle_digest(sources)}"}};'
    )

    for index, source in enumerate(sources):
        append("")
        append(f"        // {source.relative_path}")
        append(f"        constexpr auto k_source{index} = std::string_view{{")
        append(render_source_literal(source, " " * 12))
        append("        };")

    append("")
    append(
        f"        constexpr auto k_entries = std::array<FrameworkBundleEntry, {len(sources)}>{{"
    )
    for index, source in enumerate(sources):
        append("            FrameworkBundleEntry{")
        append(f'                .name       = std::string_view{{"{source.name}"}},')
        append(f"                .source     = k_source{index},")
        append(f'                .sourceHash = std::string_view{{"{source.digest}"}},')
        append("            },")
    append("        };")
    append("    }")
    append("")
    append("    auto frameworkBundleEntries() noexcept -> std::span<FrameworkBundleEntry const>")
    append("    {")
    append("        return std::span<FrameworkBundleEntry const>{k_entries};")
    append("    }")
    append("")
    append("    auto frameworkBundleHash() noexcept -> std::string_view")
    append("    {")
    append("        return k_bundleHash;")
    append("    }")
    append("")
    append("    auto frameworkVersion() noexcept -> std::string_view")
    append("    {")
    append("        return k_frameworkVersion;")
    append("    }")
    append("}")

    return "\n".join(lines) + "\n"


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--source-dir",
        type=Path,
        required=True,
        help="directory scanned recursively for .luau sources",
    )
    argument_parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="generated C++ translation unit, written into the build tree",
    )
    argument_parser.add_argument(
        "--version",
        required=True,
        help="framework semantic version stamped into the bundle",
    )
    argument_parser.add_argument(
        "--label",
        default="",
        help=(
            "repository-relative name of the source directory, used only in the "
            "generated file's header comment; keeping the absolute path out of the "
            "output is what makes it byte-identical across machines"
        ),
    )
    arguments = argument_parser.parse_args()

    source_dir = arguments.source_dir.resolve()
    if not source_dir.is_dir():
        print(f"embed_luau: not a directory: {source_dir}", file=sys.stderr)
        return 1

    try:
        sources = collect_sources(source_dir)
    except EmbedError as error:
        print(f"embed_luau: {error}", file=sys.stderr)
        return 1

    label = arguments.label or source_dir.name
    content = render(sources, arguments.version, label)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(content, encoding="utf-8", newline="\n")

    print(f"embed_luau: embedded {len(sources)} Luau module(s) into {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
