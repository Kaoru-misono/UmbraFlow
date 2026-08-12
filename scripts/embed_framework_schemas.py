#!/usr/bin/env python3
"""Generate the deployment module's framework schema catalog from schema/.

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
            f'                .exactBytes   = R"{delimiter}({text}){delimiter}",',
            "            },",
        )
    )


def main() -> None:
    args = parse_args()
    entries = "\n".join(
        render_entry(args.schema_dir, relative_path)
        for relative_path in sorted(args.schemas)
    )
    output = f'''#include <deployment/framework-schema-catalog.hpp>

#include <array>
#include <span>

namespace uf::deployment
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
