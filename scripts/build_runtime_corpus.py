#!/usr/bin/env python3
"""Compile exported game-database rows into a canonical runtime JSON resource."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def compile_corpus(source: Path) -> bytes:
    with source.open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows or set(rows[0]) != {"enabled", "region", "sequence"}:
        raise ValueError("runtime corpus input must have enabled, region, sequence columns")

    enabled: list[tuple[int, str]] = []
    sequences: set[int] = set()
    for row in rows:
        sequence = int(row["sequence"])
        if sequence in sequences:
            raise ValueError(f"runtime corpus sequence appears more than once: {sequence}")
        sequences.add(sequence)
        if row["enabled"] not in {"false", "true"}:
            raise ValueError("runtime corpus enabled values must be true or false")
        if row["enabled"] == "true":
            if not row["region"]:
                raise ValueError("enabled runtime corpus rows require a region")
            enabled.append((sequence, row["region"]))

    payload = {
        "regions": [region for _, region in sorted(enabled)],
        "schema": "arcana-expedition/map-v1",
    }
    return json.dumps(
        payload,
        ensure_ascii=False,
        separators=(",", ":"),
        sort_keys=True,
    ).encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    output = compile_corpus(args.input)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
