#!/usr/bin/env python3
"""Publish a Project Kit release bundle manifest.

This script is the only writer of a release manifest. Its output is the
immutable ``umbraflow-release/v1`` document that
``docs/decisions/2026-08-19-project-kit-distribution.md`` rules, and the
constants below are the shape authority: a template's downloader (consumer
side) parses these exact members, selects the artifact for its host platform
and arch, and refuses a mismatch on the declared sha256. The framework schema
catalog does not carry this document -- no C++ reader consumes it -- so the
wire tag is owned by this script, exactly as the project kit's own manifests
are (``umbraflow-project-kit-artifact-manifest/v1`` and
``umbraflow-project-kit-execution-closure/v1`` have no schema file).

The release id is the sha256 of the manifest's canonical bytes, derived here
and printed, never stored in the manifest: a digest inside its own document
would be a self-referential copy of bytes that are already present, the same
reason the project release derives its id from its artifact manifest
(``freezeProject`` in ``modules/project/source/project/project-kit.cpp``).
The manifest is written in RFC 8785 JCS form with no trailing newline, so the
id is a function of the exact bytes on disk.

An artifact ``path`` is canonical, ``'/'``-only and relative to the release
root, on the same terms the project manifest's path discipline demands: the
publisher writes it and the downloader refuses anything else.

Run after a green local gate. This script computes and publishes; it does not
verify the gate.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import platform as host_platform
import sys
from pathlib import Path

RELEASE_MANIFEST_SCHEMA = "umbraflow-release/v1"

# The format versions this release's tooling understands. A template declares
# the project contract it targets; the downloader picks a release whose list
# carries it. Compatibility selection uses format versions, never digests.
RELEASE_MANIFEST_CONTRACT_VERSIONS = (
    "umbraflow-project/v2",
    "umbraflow-project-kit-artifact-manifest/v1",
)

# The top-level members of the release manifest and of one artifact row.
# The generator reads these exact tuples into the public contract, so a member
# added here must also gain a meaning in scripts/generate_public_contract.py.
RELEASE_MANIFEST_MEMBERS = ("schema", "release", "contract_versions", "artifacts")
RELEASE_ARTIFACT_MEMBERS = (
    "name",
    "platform",
    "arch",
    "path",
    "asset",
    "sha256",
)

# The binaries a release carries, by logical name. The platform executable
# suffix is derived, not stored here.
RELEASE_BINARIES = ("project", "umbra-flow", "umbra-flow-conformance")

# Runtime payload patterns relative to the release root (the bin directory).
# The DLLs must sit beside the executables, because Windows loads a DLL from
# the executable's own directory, and the OCR models under models/ the way
# umbra-flow answers `--ocr-models <bin>/models`. Each matched file becomes
# one artifact row; the path in the manifest is the file's path relative to
# the release root, which is what the downloader restores.
RELEASE_PAYLOAD_PATTERNS = ("onnxruntime*.dll", "models/**/*")

PLATFORMS = ("windows", "linux", "macos")
ARCHES = ("x64", "arm64")


def detect_platform() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    if sys.platform.startswith("darwin"):
        return "macos"
    raise SystemExit(f"unsupported host platform: {sys.platform}")


def detect_arch() -> str:
    machine = host_platform.machine().lower()
    if machine in ("amd64", "x86_64", "x64"):
        return "x64"
    if machine in ("arm64", "aarch64"):
        return "arm64"
    raise SystemExit(f"unsupported host architecture: {host_platform.machine()}")


def executable_suffix(platform_name: str) -> str:
    if platform_name == "windows":
        return ".exe"
    return ""


def digest_of(path: Path) -> str:
    """Bare lowercase hex content digest, the spelling every published
    document uses; a reader adds the ``sha256:`` prefix ContentHash::parse
    wants."""
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_rows(
    bin_dir: Path,
    names: tuple[str, ...],
    platform_name: str,
    arch: str,
) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for name in names:
        filename = f"{name}{executable_suffix(platform_name)}"
        path = bin_dir / filename
        if not path.is_file():
            raise SystemExit(f"release artifact is missing: {path}")
        rows.append(
            {
                "name": name,
                "platform": platform_name,
                "arch": arch,
                "path": filename,
                "asset": filename,
                "sha256": digest_of(path),
            }
        )
    for pattern in RELEASE_PAYLOAD_PATTERNS:
        for path in sorted(bin_dir.glob(pattern)):
            if not path.is_file():
                continue
            relative = path.relative_to(bin_dir).as_posix()
            rows.append(
                {
                    "name": relative,
                    "platform": platform_name,
                    "arch": arch,
                    "path": relative,
                    # GitHub release assets are flat, so a nested payload file
                    # is uploaded under a name without '/'; the downloader
                    # restores it at its path, not its asset name.
                    "asset": relative.replace("/", "-"),
                    "sha256": digest_of(path),
                }
            )
    return rows


def build_manifest(
    bin_dir: Path,
    release: str,
    platform_name: str,
    arch: str,
    names: tuple[str, ...],
) -> dict[str, object]:
    return {
        "schema": RELEASE_MANIFEST_SCHEMA,
        "release": release,
        "contract_versions": list(RELEASE_MANIFEST_CONTRACT_VERSIONS),
        "artifacts": artifact_rows(bin_dir, names, platform_name, arch),
    }


def canonical_json(value: object) -> str:
    """RFC 8785 JCS for the ASCII-only content of this manifest: sorted object
    members, compact separators, no unnecessary escaping."""
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=True,
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bin-dir",
        required=True,
        type=Path,
        help="directory holding the built binaries (CMake runtime output dir)",
    )
    parser.add_argument(
        "--release",
        required=True,
        help="milestone name, e.g. m0-acceptance; not semver while no packaged release exists",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("umbraflow-release-v1.json"),
        help="where to write the manifest (default: umbraflow-release-v1.json)",
    )
    parser.add_argument(
        "--platform",
        choices=sorted(PLATFORMS),
        default=detect_platform(),
        help="artifact platform (default: the host)",
    )
    parser.add_argument(
        "--arch",
        choices=sorted(ARCHES),
        default=detect_arch(),
        help="artifact architecture (default: the host)",
    )
    parser.add_argument(
        "--names",
        nargs="+",
        default=list(RELEASE_BINARIES),
        help="logical binary names to include (default: the shipped set)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    bin_dir = args.bin_dir.resolve()
    if not bin_dir.is_dir():
        raise SystemExit(f"release bin directory does not exist: {bin_dir}")
    if not args.release:
        raise SystemExit("release requires a milestone name")
    if not args.names:
        raise SystemExit("release requires at least one binary name")

    manifest = build_manifest(
        bin_dir,
        args.release,
        args.platform,
        args.arch,
        tuple(args.names),
    )
    encoded = canonical_json(manifest).encode("utf-8")
    args.output.write_bytes(encoded)
    release_id = hashlib.sha256(encoded).hexdigest()
    print(
        f"release id: sha256:{release_id} "
        f"release={args.release} platform={args.platform}/{args.arch} "
        f"artifacts={len(manifest['artifacts'])}"
    )
    print(f"release manifest ({len(encoded)} bytes): {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
