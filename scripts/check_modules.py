#!/usr/bin/env python3
"""Validate the first-party dependency graph and the platform-free core boundary."""

from __future__ import annotations

import argparse
import configparser
import re
import sys
from dataclasses import dataclass
from pathlib import Path


DEPENDENCY_SECTION_PREFIX = "dependencies"

# The one root, and every child of it carrying a manifest is one library the
# CMake autoloader builds; cmake/build.cmake requires the source/<directory>
# layout enforced below. Nothing outside this directory declares a manifest, so
# the graph below is all the first-party C++ that can close a cycle. entry/ and
# tests/ cannot: they compile no library and nothing links them.
MODULE_ROOT = "modules"

# [embed] declares non-C++ sources compiled into the module. Today the only
# embeddable kind is Luau: luau_directory names a module-relative tree of .luau
# files, luau_version the semantic version stamped into the generated bundle.
# cmake/build.cmake runs scripts/embed_luau.py over that tree at build time.
EMBED_SECTION = "embed"
SEMANTIC_VERSION = re.compile(r"^\d+\.\d+\.\d+$")


@dataclass(frozen=True)
class Module:
    name: str
    directory: str
    manifest: Path
    dependencies: tuple[str, ...]
    embed_luau_directory: str
    embed_luau_version: str


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
        embed_luau_directory=parser.get(
            EMBED_SECTION, "luau_directory", fallback=""
        ).strip(),
        embed_luau_version=parser.get(EMBED_SECTION, "luau_version", fallback="").strip(),
    )


def embed_errors(module: Module, root: Path) -> list[str]:
    """Validate an [embed] section, which the build turns into generated C++."""
    if not module.embed_luau_directory and not module.embed_luau_version:
        return []

    manifest = module.manifest.relative_to(root)
    errors: list[str] = []

    if not module.embed_luau_directory:
        errors.append(f"{manifest}: [embed] declares luau_version without luau_directory")
    else:
        directory = module.manifest.parent / module.embed_luau_directory
        if not directory.is_dir():
            errors.append(
                f"{manifest}: [embed].luau_directory {module.embed_luau_directory!r} "
                "is not a directory"
            )
        elif not any(directory.rglob("*.luau")):
            errors.append(
                f"{manifest}: [embed].luau_directory {module.embed_luau_directory!r} "
                "contains no .luau sources"
            )

    if not SEMANTIC_VERSION.match(module.embed_luau_version):
        errors.append(
            f"{manifest}: [embed].luau_version must be MAJOR.MINOR.PATCH, "
            f"got {module.embed_luau_version!r}"
        )

    return errors


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
    module_root = root / MODULE_ROOT
    errors: list[str] = []

    modules = [
        load_module(manifest)
        for manifest in sorted(module_root.glob("*/manifest.txt"))
    ]

    modules_by_name: dict[str, Module] = {}
    for module in modules:
        previous = modules_by_name.get(module.name)
        if previous is not None:
            errors.append(
                f"duplicate module name {module.name!r}: "
                f"{previous.manifest.relative_to(root)} and {module.manifest.relative_to(root)}"
            )
        modules_by_name[module.name] = module

        # The autoloader's layout: cmake/build.cmake publishes both source/ and
        # source/<directory> as include roots, and the second is what makes
        # <module/header.hpp> resolve.
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

        errors.extend(embed_errors(module, root))

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

    print(f"Module graph check OK ({len(modules)} modules under {MODULE_ROOT}/).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
