#!/usr/bin/env python3
"""Validate the local module graph and the platform-free core boundary."""

from __future__ import annotations

import argparse
import configparser
import re
import sys
from dataclasses import dataclass
from pathlib import Path


DEPENDENCY_SECTION_PREFIX = "dependencies"


@dataclass(frozen=True)
class Module:
    name: str
    directory: str
    manifest: Path
    dependencies: tuple[str, ...]


def split_dependencies(value: str) -> list[str]:
    return [item for item in re.split(r"[,\s]+", value) if item]


def load_module(manifest: Path) -> Module:
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(manifest, encoding="utf-8")

    directory = manifest.parent.name
    name = parser.get("module", "name", fallback=directory).strip() or directory
    dependencies: list[str] = []
    for section in parser.sections():
        if not section.lower().startswith(DEPENDENCY_SECTION_PREFIX):
            continue
        for key in ("public", "private"):
            dependencies.extend(split_dependencies(parser.get(section, key, fallback="")))

    return Module(
        name=name,
        directory=directory,
        manifest=manifest,
        dependencies=tuple(dict.fromkeys(dependencies)),
    )


def find_cycle(graph: dict[str, tuple[str, ...]]) -> list[str] | None:
    visited: set[str] = set()
    active: set[str] = set()
    path: list[str] = []

    def visit(module: str) -> list[str] | None:
        if module in active:
            start = path.index(module)
            return [*path[start:], module]
        if module in visited:
            return None

        active.add(module)
        path.append(module)
        for dependency in graph[module]:
            cycle = visit(dependency)
            if cycle is not None:
                return cycle
        path.pop()
        active.remove(module)
        visited.add(module)
        return None

    for module in graph:
        cycle = visit(module)
        if cycle is not None:
            return cycle
    return None


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
    module_root = root / "modules"
    manifests = sorted(module_root.glob("*/manifest.txt"))
    modules = [load_module(manifest) for manifest in manifests]
    errors: list[str] = []

    modules_by_name: dict[str, Module] = {}
    for module in modules:
        previous = modules_by_name.get(module.name)
        if previous is not None:
            errors.append(
                f"duplicate module name {module.name!r}: "
                f"{previous.manifest.relative_to(root)} and {module.manifest.relative_to(root)}"
            )
        modules_by_name[module.name] = module

        expected_source = module.manifest.parent / "source" / module.directory
        if not expected_source.is_dir():
            errors.append(
                f"{module.manifest.relative_to(root)}: expected source directory "
                f"{expected_source.relative_to(root)}"
            )

        if module.name == "core" and module.dependencies:
            errors.append(
                f"{module.manifest.relative_to(root)}: core must not declare link dependencies"
            )

    if "core" not in modules_by_name:
        errors.append("required core module is missing")

    local_names = set(modules_by_name)
    graph: dict[str, tuple[str, ...]] = {}
    for module in modules:
        local_dependencies = tuple(
            dependency for dependency in module.dependencies if dependency in local_names
        )
        if module.name in local_dependencies:
            errors.append(f"{module.manifest.relative_to(root)}: module depends on itself")
        graph[module.name] = local_dependencies

    cycle = find_cycle(graph)
    if cycle is not None:
        errors.append(f"module dependency cycle: {' -> '.join(cycle)}")

    if errors:
        print("Module graph violations:", file=sys.stderr)
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    print(f"Module graph check OK ({len(modules)} modules).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
