#!/usr/bin/env python3
"""Verify the frozen specification bundle against this repository's root pin.

The bundle is the normative product input named by
docs/plans/2026-08-09-runtime-hardening-rewrite.md. It lives in a consumer
repository this one may not modify. The check exits non-zero when it cannot read
that bundle.

The root fixes ``spec-bundle.manifest.json``. That manifest fixes every member's
path, byte size and SHA-256, which this verifier checks against the real bundle.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


AUTHORITY_DOCUMENT = PurePosixPath("docs/plans/2026-08-09-runtime-hardening-rewrite.md")

BUNDLE_LOCATION = re.compile(r"read-only v\d+\.\d+ bundle at\s+`(?P<path>[^`]+)`")

CRLF_REASON = "line endings were translated to CRLF; the pin is over the LF bytes"

BUNDLE_FILE_SUFFIXES = {".json", ".md"}


@dataclass(frozen=True)
class BundleSpec:
    manifest_name: str
    root_digest: str


FROZEN_BUNDLE = BundleSpec(
    manifest_name="spec-bundle.manifest.json",
    root_digest="ac8c3fa652fb1601645d0c0bc04359bc75c9d08dc2883aa31ddeb94912f38ec4",
)


@dataclass(frozen=True)
class BundleReport:
    violations: tuple[str, ...]
    version: str
    file_count: int
    byte_count: int
    unlisted: tuple[str, ...]


def digest_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_mismatch_reason(data: bytes, pinned: str) -> str:
    if b"\r\n" in data and digest_of(data.replace(b"\r\n", b"\n")) == pinned:
        return CRLF_REASON
    return "content differs from the pinned bytes"


def verify_bundle(directory: Path, spec: BundleSpec) -> BundleReport:
    if not directory.is_dir():
        return BundleReport(
            violations=(f"bundle directory does not exist: {directory}",),
            version="unknown",
            file_count=0,
            byte_count=0,
            unlisted=(),
        )

    violations: list[str] = []
    listed = {spec.manifest_name}
    file_count = 0
    byte_count = 0
    manifest_path = directory / spec.manifest_name
    try:
        manifest_bytes = manifest_path.read_bytes()
    except OSError as error:
        return BundleReport(
            violations=(
                f"{spec.manifest_name}: cannot be read ({error.strerror or error})",
            ),
            version="unknown",
            file_count=0,
            byte_count=0,
            unlisted=(),
        )

    file_count += 1
    byte_count += len(manifest_bytes)
    actual_root = digest_of(manifest_bytes)
    if actual_root != spec.root_digest:
        violations.append(
            f"{spec.manifest_name}: bundle root is {actual_root}, pinned {spec.root_digest} "
            f"-- {digest_mismatch_reason(manifest_bytes, spec.root_digest)}"
        )

    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return BundleReport(
            violations=tuple(
                [*violations, f"{spec.manifest_name}: is not readable JSON ({error})"]
            ),
            version="unknown",
            file_count=file_count,
            byte_count=byte_count,
            unlisted=(),
        )

    if manifest.get("hash_algorithm") != "sha256":
        violations.append(
            f"{spec.manifest_name}: hash_algorithm is "
            f"{manifest.get('hash_algorithm')!r}, expected 'sha256'"
        )

    version_value = manifest.get("bundle_version")
    version = version_value if isinstance(version_value, str) else "unknown"
    if version == "unknown":
        violations.append(f"{spec.manifest_name}: bundle_version is not a string")

    entries = manifest.get("documents")
    if not isinstance(entries, list):
        violations.append(f"{spec.manifest_name}: documents is not a list")
        entries = []

    for entry in entries:
        declared = PurePosixPath(str(entry.get("path", "")))
        name = declared.name
        listed.add(name)

        expected_tail = tuple(part.lower() for part in declared.parent.parts)
        actual_tail = tuple(part.lower() for part in directory.parts[-len(expected_tail) :])
        if expected_tail and expected_tail != actual_tail:
            violations.append(
                f"{spec.manifest_name}: {name} is declared under "
                f"{declared.parent.as_posix()!r}, but the bundle was read from {directory}"
            )

        declared_digest = entry.get("sha256")
        try:
            data = (directory / name).read_bytes()
        except OSError as error:
            violations.append(f"{name}: cannot be read ({error.strerror or error})")
            continue

        file_count += 1
        byte_count += len(data)
        declared_size = entry.get("byte_size")
        if declared_size != len(data):
            violations.append(
                f"{spec.manifest_name}: {name} is declared as {declared_size!r} bytes, "
                f"on disk it is {len(data)}"
            )

        actual_digest = digest_of(data)
        if actual_digest != declared_digest:
            violations.append(
                f"{name}: SHA-256 is {actual_digest}, manifest declares "
                f"{declared_digest} -- {digest_mismatch_reason(data, declared_digest)}"
            )

    if not violations and file_count != len(entries) + 1:
        violations.append(
            f"internal: {file_count} of {len(entries) + 1} manifest-listed files were hashed"
        )

    unlisted = tuple(
        sorted(
            path.name
            for path in directory.iterdir()
            if path.is_file()
            and path.suffix.lower() in BUNDLE_FILE_SUFFIXES
            and path.name not in listed
        )
    )

    return BundleReport(
        violations=tuple(violations),
        version=version,
        file_count=file_count,
        byte_count=byte_count,
        unlisted=unlisted,
    )


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    argument_parser.add_argument(
        "--bundle",
        type=Path,
        help="the frozen bundle directory; defaults to the location the authority document names",
    )
    arguments = argument_parser.parse_args()

    if arguments.bundle is None:
        root = arguments.root.resolve()
        if not root.is_dir():
            argument_parser.error(f"repository root does not exist: {root}")

        authority = root / AUTHORITY_DOCUMENT
        try:
            text = authority.read_text(encoding="utf-8")
        except OSError as error:
            print("Spec bundle check could not run:", file=sys.stderr)
            print(
                f"  {AUTHORITY_DOCUMENT.as_posix()}: cannot be read ({error})",
                file=sys.stderr,
            )
            print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
            return 2

        location = BUNDLE_LOCATION.search(text)
        if location is None:
            print("Spec bundle check could not run:", file=sys.stderr)
            print(
                f"  {AUTHORITY_DOCUMENT.as_posix()}: no sentence states the bundle location",
                file=sys.stderr,
            )
            print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
            return 2
        bundle_directory = Path(location.group("path")).resolve()
    else:
        bundle_directory = arguments.bundle.resolve()

    if not bundle_directory.is_dir():
        print("Spec bundle check could not run:", file=sys.stderr)
        print(f"  bundle directory does not exist: {bundle_directory}", file=sys.stderr)
        print("  pass --bundle <directory> containing the real bundle", file=sys.stderr)
        print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
        return 2

    report = verify_bundle(bundle_directory, FROZEN_BUNDLE)
    if report.violations:
        print("Spec bundle violations:", file=sys.stderr)
        for violation in report.violations:
            print(f"  {violation}", file=sys.stderr)
        print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
        return 1

    print(
        f"Spec bundle check OK ({report.file_count} files, "
        f"{report.byte_count} bytes hashed)."
    )
    if report.unlisted:
        print(f"  outside the pinned bundle, not checked: {', '.join(report.unlisted)}")
    print(
        f"SPEC BUNDLE: VERIFIED (v{report.version} root "
        f"{FROZEN_BUNDLE.root_digest}, {bundle_directory})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
