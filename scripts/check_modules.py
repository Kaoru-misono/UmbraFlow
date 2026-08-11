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

# Every child of this directory carrying a manifest is one library the CMake
# autoloader builds, and cmake/build.cmake requires the source/<directory>
# layout enforced below.
AUTOLOADED_MODULE_ROOT = "modules"

# First-party C++ that declares a manifest without being one of those. The
# autoloader never reaches these -- CPP_MODULE_ROOTS names modules/ only -- but
# they take dependencies on modules, so leaving them out of the graph means a
# dependency that closes a cycle passes while the count still reads OK. The
# count was correct and was never all the C++.
#
# tests/support/ is in this position too and is not listed: it declares no
# manifest yet, so its dependencies are still unchecked.
DECLARED_SOURCE_TREES = ("conformance",)

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
    autoloaded: bool


def split_dependencies(value: str) -> list[str]:
    return [item for item in re.split(r"[,\s]+", value) if item]


def load_module(manifest: Path, autoloaded: bool) -> Module:
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
        autoloaded=autoloaded,
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
    module_root = root / AUTOLOADED_MODULE_ROOT
    errors: list[str] = []

    modules = [
        load_module(manifest, autoloaded=True)
        for manifest in sorted(module_root.glob("*/manifest.txt"))
    ]
    for tree in DECLARED_SOURCE_TREES:
        manifest = root / tree / "manifest.txt"
        if not manifest.is_file():
            errors.append(f"{tree}/manifest.txt is missing")
            continue
        modules.append(load_module(manifest, autoloaded=False))

    modules_by_name: dict[str, Module] = {}
    for module in modules:
        previous = modules_by_name.get(module.name)
        if previous is not None:
            errors.append(
                f"duplicate module name {module.name!r}: "
                f"{previous.manifest.relative_to(root)} and {module.manifest.relative_to(root)}"
            )
        modules_by_name[module.name] = module

        # The autoloader's layout, and only the autoloader's: cmake/build.cmake
        # publishes source/<directory> as an include root. A tree it never sees
        # owes only that its sources are where the manifest says the tree is.
        expected_source = module.manifest.parent / "source"
        if module.autoloaded:
            expected_source = expected_source / module.directory
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

    autoloaded = sum(1 for module in modules if module.autoloaded)
    print(
        f"Module graph check OK ({len(modules)} manifests: {autoloaded} modules "
        f"under {AUTOLOADED_MODULE_ROOT}/, {len(modules) - autoloaded} outside it)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
