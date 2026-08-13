#!/usr/bin/env python3
"""Generate the framework schema module's runtime catalog from schema/.

The published documents are carried as their exact bytes and as the sha256 of
those bytes, so the runtime catalog and the file under schema/ cannot drift:
there is one authored spelling of each document and the digest is recomputed
from it at every build.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("schemas", nargs="+")
    return parser.parse_args()


# MSVC refuses a single string literal longer than 16380 bytes, and adjacent
# literals concatenate with no such limit. The chunk is well under that so a
# multi-byte character straddling a boundary still leaves both halves short
# enough; splitting on a code point rather than a byte keeps each literal valid
# UTF-8 on its own.
_LITERAL_CHUNK = 4000


def render_exact_bytes(text: str, delimiter: str) -> str:
    if len(text.encode("utf-8")) <= _LITERAL_CHUNK:
        return f'R"{delimiter}({text}){delimiter}"'
    chunks: list[str] = []
    current: list[str] = []
    size = 0
    for character in text:
        width = len(character.encode("utf-8"))
        if size + width > _LITERAL_CHUNK:
            chunks.append("".join(current))
            current, size = [], 0
        current.append(character)
        size += width
    if current:
        chunks.append("".join(current))
    joined = "\n                                ".join(
        f'R"{delimiter}({chunk}){delimiter}"' for chunk in chunks
    )
    return joined


def render_entry(schema_dir: Path, relative_path: str) -> str:
    path = schema_dir / relative_path
    exact_bytes = path.read_bytes()
    document = json.loads(exact_bytes)
    identity = document["$id"]
    digest = hashlib.sha256(exact_bytes).hexdigest()
    text = exact_bytes.decode("utf-8")
    delimiter = "uf_schema"
    if f"){delimiter}\"" in text:
        raise ValueError(f"{relative_path} contains the raw-string delimiter")
    return "\n".join(
        (
            "            FrameworkSchemaDocument{",
            f'                .identity     = "{identity}",',
            f'                .relativePath = "schema/{relative_path}",',
            f'                .sha256       = "{digest}",',
            f"                .exactBytes   = {render_exact_bytes(text, delimiter)},",
            "            },",
        )
    )


def main() -> None:
    args = parse_args()
    entries = "\n".join(
        render_entry(args.schema_dir, relative_path)
        for relative_path in sorted(args.schemas)
    )
    output = f'''#include <schema/framework-schema-catalog.hpp>

#include <array>
#include <span>

namespace uf::framework_schema
{{
    auto frameworkSchemaCatalog() noexcept
        -> std::span<FrameworkSchemaDocument const>
    {{
        static constexpr auto k_documents = std::array{{
{entries}
        }};
        return k_documents;
    }}
}}
'''
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(output, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
