#!/usr/bin/env python3
"""Verify the frozen specification bundle against the digests this repository pins.

The bundle is the normative product input named by
docs/plans/2026-08-09-runtime-hardening-rewrite.md. It lives in a consumer
repository this one may not modify, at an absolute path that exists on one
machine, so "the bundle is not here" is a normal condition rather than an error
to paper over. It is never a pass: without ``--pins-only`` the check exits
non-zero when it cannot read the bundle, and with ``--pins-only`` it verifies
only what this repository states about the bundle and says on its own success
line that the bundle was not read.

What the pinned digests are computed over: SHA-256 of each file's exact bytes as
stored, read in binary with no newline translation. Two facts fix that reading
and both are checked here rather than trusted -- every bundle file is LF-only,
and ``byte_size`` in the manifest equals the on-disk size. The consumer
repository carries ``* -text`` in .gitattributes, so a checkout cannot rewrite
those bytes; a checkout that did would fail every digest at once, which is why
CRLF is diagnosed by name below instead of being reported as a content change. A
false content alarm is what teaches a reader to ignore a gate.

The bundle root is SHA-256 of spec-bundle.manifest.json itself. The authority
document states that as a number; this script states it as a rule and checks it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import shutil
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath


AUTHORITY_DOCUMENT = PurePosixPath("docs/plans/2026-08-09-runtime-hardening-rewrite.md")

# The location and version live in one prose sentence of the authority document.
# Parsing them keeps the gate and the document from disagreeing silently: a
# reshaped sentence stops the run rather than falling back to a stale default.
BUNDLE_LOCATION = re.compile(r"read-only v(?P<version>\d+\.\d+) bundle at\s+`(?P<path>[^`]+)`")

CRLF_REASON = "line endings were translated to CRLF; the pin is over the LF bytes"

BUNDLE_FILE_SUFFIXES = {".json", ".md"}


@dataclass(frozen=True)
class Pin:
    """One pinned file: the label the authority document states it under, and its digest."""

    label: str
    file_name: str
    digest: str


@dataclass(frozen=True)
class BundleSpec:
    version: str
    manifest_name: str
    pins: tuple[Pin, ...]


# Pinned here as well as in the authority document, deliberately. A gate that
# read its expectation out of the document it checks would agree with any edit
# to that document; holding two independent copies is what makes an altered pin
# fail rather than propagate.
FROZEN_BUNDLE = BundleSpec(
    version="1.10",
    manifest_name="spec-bundle.manifest.json",
    pins=(
        Pin(
            "bundle root SHA-256",
            "spec-bundle.manifest.json",
            "adb7f29f52dc2c0217d888f8d4da815d335db480db64191f0c4e873bda51049f",
        ),
        Pin(
            "main design",
            "umbraflow-game-automation-final-design.md",
            "3499e87580c0dd9690e5ce5bace446b0198d6270d4e4706d1f6accb575ee0b44",
        ),
        Pin(
            "project-layer design",
            "uf-chaos-project-layer-design.md",
            "bb4fa64165c61bddfb795a1a0b8cc6158bea6669bf0ef5c45a40fd4986d77177",
        ),
        Pin(
            "requirements",
            "requirements-traceability.md",
            "2b725e81ffcdac30f38dcd9f77cb2513dca6251b3fdbb1cbe42346324724196c",
        ),
        Pin(
            "failure/recovery audit",
            "failure-and-recovery-audit.md",
            "aad291c97157fd8e6c63a3c05b1bf91361188aa8353f783795548a1f80b00e55",
        ),
    ),
)

SELF_TEST_CONTROLS = (
    "an intact bundle passes",
    "one flipped byte fails",
    "an emptied document fails",
    "a missing document fails",
    "a CRLF checkout fails and is named as such",
    "a missing bundle directory fails",
)


@dataclass(frozen=True)
class BundleReport:
    violations: tuple[str, ...]
    file_count: int
    byte_count: int
    unlisted: tuple[str, ...]


def digest_of(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def digest_mismatch_reason(data: bytes, pinned: str) -> str:
    if b"\r\n" in data and digest_of(data.replace(b"\r\n", b"\n")) == pinned:
        return CRLF_REASON
    return "content differs from the pinned bytes"


def document_pins(spec: BundleSpec) -> tuple[Pin, ...]:
    return tuple(pin for pin in spec.pins if pin.file_name != spec.manifest_name)


def manifest_violations(
    manifest_bytes: bytes,
    directory: Path,
    spec: BundleSpec,
    contents: dict[str, bytes],
) -> list[str]:
    """Check the manifest's own claims about the documents the root digest covers.

    Subordinate to the root digest, which already fails on any manifest byte
    change. It exists to name which document drifted, and to keep ``path`` and
    ``byte_size`` read rather than merely written.
    """
    violations: list[str] = []
    try:
        manifest = json.loads(manifest_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        return [f"{spec.manifest_name}: is not readable JSON ({error})"]

    if manifest.get("hash_algorithm") != "sha256":
        violations.append(
            f"{spec.manifest_name}: hash_algorithm is "
            f"{manifest.get('hash_algorithm')!r}, expected 'sha256'"
        )
    if manifest.get("bundle_version") != spec.version:
        violations.append(
            f"{spec.manifest_name}: bundle_version is "
            f"{manifest.get('bundle_version')!r}, this gate pins {spec.version!r}"
        )

    entries = manifest.get("documents")
    if not isinstance(entries, list):
        violations.append(f"{spec.manifest_name}: documents is not a list")
        return violations

    pins_by_name = {pin.file_name: pin for pin in document_pins(spec)}
    listed: list[str] = []
    for entry in entries:
        declared = PurePosixPath(str(entry.get("path", "")))
        name = declared.name
        listed.append(name)
        pin = pins_by_name.get(name)
        if pin is None:
            violations.append(
                f"{spec.manifest_name}: documents lists {declared.as_posix()!r}, "
                "which this gate does not pin"
            )
            continue

        # The declared path is repository-relative in the consumer repository, so
        # its directory must be the tail of wherever the bundle was found. This is
        # what makes a partial copy into a differently named directory visible.
        expected_tail = tuple(part.lower() for part in declared.parent.parts)
        actual_tail = tuple(part.lower() for part in directory.parts[-len(expected_tail) :])
        if expected_tail and expected_tail != actual_tail:
            violations.append(
                f"{spec.manifest_name}: {name} is declared under "
                f"{declared.parent.as_posix()!r}, but the bundle was read from {directory}"
            )

        if entry.get("sha256") != pin.digest:
            violations.append(
                f"{spec.manifest_name}: {name} is declared as {entry.get('sha256')!r}, "
                f"this gate pins {pin.digest}"
            )

        data = contents.get(name)
        if data is not None and entry.get("byte_size") != len(data):
            violations.append(
                f"{spec.manifest_name}: {name} is declared as "
                f"{entry.get('byte_size')!r} bytes, on disk it is {len(data)}"
            )

    missing = sorted(set(pins_by_name) - set(listed))
    for name in missing:
        violations.append(f"{spec.manifest_name}: documents does not list {name}")

    return violations


def verify_bundle(directory: Path, spec: BundleSpec) -> BundleReport:
    if not directory.is_dir():
        return BundleReport(
            violations=(f"bundle directory does not exist: {directory}",),
            file_count=0,
            byte_count=0,
            unlisted=(),
        )

    violations: list[str] = []
    contents: dict[str, bytes] = {}
    byte_count = 0

    for pin in spec.pins:
        path = directory / pin.file_name
        try:
            data = path.read_bytes()
        except OSError as error:
            violations.append(f"{pin.file_name}: cannot be read ({error.strerror or error})")
            continue

        contents[pin.file_name] = data
        byte_count += len(data)
        actual = digest_of(data)
        if actual != pin.digest:
            violations.append(
                f"{pin.file_name}: {pin.label} is {actual}, pinned {pin.digest} "
                f"-- {digest_mismatch_reason(data, pin.digest)}"
            )

    manifest_bytes = contents.get(spec.manifest_name)
    if manifest_bytes is not None:
        violations.extend(manifest_violations(manifest_bytes, directory, spec, contents))

    # A pass must name the files it hashed. Without this the loop above could
    # skip a pin through a future edit and report nothing, which is the shape
    # docs/pitfalls/checks-that-cannot-fail.md collects.
    if not violations and len(contents) != len(spec.pins):
        violations.append(
            f"internal: {len(contents)} of {len(spec.pins)} pinned files were hashed"
        )

    # Reported, never failed on. The authority pins five files; it does not claim
    # the directory holds nothing else, and failing on a consumer's unrelated
    # note would be a false alarm about bytes that did not change.
    listed = {pin.file_name for pin in spec.pins}
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
        file_count=len(contents),
        byte_count=byte_count,
        unlisted=unlisted,
    )


def write_synthetic_bundle(root: Path) -> tuple[Path, BundleSpec]:
    directory = root / "docs" / "architecture"
    directory.mkdir(parents=True)

    document_name = "probe-design.md"
    document = b"# probe\n\nfrozen line\n"
    (directory / document_name).write_bytes(document)

    manifest_name = "spec-bundle.manifest.json"
    manifest = (
        json.dumps(
            {
                "bundle_version": "0.0",
                "hash_algorithm": "sha256",
                "documents": [
                    {
                        "path": f"docs/architecture/{document_name}",
                        "byte_size": len(document),
                        "sha256": digest_of(document),
                    }
                ],
            },
            indent=2,
        ).encode("utf-8")
        + b"\n"
    )
    (directory / manifest_name).write_bytes(manifest)

    return directory, BundleSpec(
        version="0.0",
        manifest_name=manifest_name,
        pins=(
            Pin("synthetic root", manifest_name, digest_of(manifest)),
            Pin("synthetic document", document_name, digest_of(document)),
        ),
    )


def self_test_violations() -> list[str]:
    """Positive control: show the verifier going red on each way a bundle drifts.

    Runs on every invocation, including --pins-only, where it is the only thing
    separating "the bundle machinery works" from an untested claim.
    """
    failures: list[str] = []

    with tempfile.TemporaryDirectory() as temporary:
        directory, spec = write_synthetic_bundle(Path(temporary))
        document = directory / spec.pins[1].file_name
        original = document.read_bytes()

        report = verify_bundle(directory, spec)
        if report.violations:
            failures.append(f"control {SELF_TEST_CONTROLS[0]!r} reported {list(report.violations)}")
        elif report.file_count != len(spec.pins):
            failures.append(
                f"control {SELF_TEST_CONTROLS[0]!r} hashed {report.file_count} files, "
                f"expected {len(spec.pins)}"
            )

        document.write_bytes(original[:-2] + b"X\n")
        if not verify_bundle(directory, spec).violations:
            failures.append(f"control {SELF_TEST_CONTROLS[1]!r} stayed green")

        document.write_bytes(b"")
        if not verify_bundle(directory, spec).violations:
            failures.append(f"control {SELF_TEST_CONTROLS[2]!r} stayed green")

        document.unlink()
        if not verify_bundle(directory, spec).violations:
            failures.append(f"control {SELF_TEST_CONTROLS[3]!r} stayed green")

        document.write_bytes(original.replace(b"\n", b"\r\n"))
        crlf = verify_bundle(directory, spec).violations
        if not crlf:
            failures.append(f"control {SELF_TEST_CONTROLS[4]!r} stayed green")
        elif not any(CRLF_REASON in violation for violation in crlf):
            failures.append(f"control {SELF_TEST_CONTROLS[4]!r} did not name the line endings")

        document.write_bytes(original)
        shutil.rmtree(directory)
        if not verify_bundle(directory, spec).violations:
            failures.append(f"control {SELF_TEST_CONTROLS[5]!r} stayed green")

    return failures


def authority_pin_violations(text: str, spec: BundleSpec) -> tuple[list[str], int]:
    violations: list[str] = []
    matched = 0
    document = AUTHORITY_DOCUMENT.as_posix()

    manifest_statement = f"- bundle manifest: `{spec.manifest_name}`"
    if manifest_statement not in text:
        violations.append(f"{document}: no line states {manifest_statement!r}")

    for pin in spec.pins:
        pattern = re.compile(rf"^- {re.escape(pin.label)}: `([0-9a-f]{{64}})`$", re.MULTILINE)
        stated = pattern.findall(text)
        if not stated:
            violations.append(f"{document}: no line states the {pin.label!r} digest")
        elif len(stated) > 1:
            violations.append(f"{document}: states the {pin.label!r} digest {len(stated)} times")
        elif stated[0] != pin.digest:
            violations.append(
                f"{document}: {pin.label!r} is stated as {stated[0]}, "
                f"this gate pins {pin.digest}"
            )
        else:
            matched += 1

    if not violations and matched != len(spec.pins):
        violations.append(f"internal: {matched} of {len(spec.pins)} pins were compared")

    return violations, matched


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root",
    )
    location_group = argument_parser.add_mutually_exclusive_group()
    location_group.add_argument(
        "--bundle",
        type=Path,
        help="the frozen bundle directory; defaults to the location the authority document names",
    )
    location_group.add_argument(
        "--pins-only",
        action="store_true",
        help="verify only this repository's pins and report the bundle as NOT VERIFIED",
    )
    arguments = argument_parser.parse_args()

    root = arguments.root.resolve()
    if not root.is_dir():
        argument_parser.error(f"repository root does not exist: {root}")

    control_failures = self_test_violations()
    if control_failures:
        print("Spec bundle verifier self-test failed:", file=sys.stderr)
        for failure in control_failures:
            print(f"  {failure}", file=sys.stderr)
        print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
        return 1

    authority = root / AUTHORITY_DOCUMENT
    try:
        text = authority.read_text(encoding="utf-8")
    except OSError as error:
        print("Spec bundle check could not run:", file=sys.stderr)
        print(f"  {AUTHORITY_DOCUMENT.as_posix()}: cannot be read ({error})", file=sys.stderr)
        print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
        return 2

    violations, matched = authority_pin_violations(text, FROZEN_BUNDLE)

    location = BUNDLE_LOCATION.search(text)
    if location is None:
        violations.append(
            f"{AUTHORITY_DOCUMENT.as_posix()}: no sentence states the bundle version and location"
        )
    elif location.group("version") != FROZEN_BUNDLE.version:
        violations.append(
            f"{AUTHORITY_DOCUMENT.as_posix()}: states bundle v{location.group('version')}, "
            f"this gate pins v{FROZEN_BUNDLE.version}"
        )

    # Pin disagreement is checkable on every host, so it is settled before the
    # bundle is looked for. Reaching the bundle can only add evidence to a
    # verdict that is already decided.
    if violations:
        print("Spec pin violations:", file=sys.stderr)
        for violation in violations:
            print(f"  {violation}", file=sys.stderr)
        print("SPEC BUNDLE: NOT VERIFIED", file=sys.stderr)
        return 1

    declared_location = location.group("path")
    controls = len(SELF_TEST_CONTROLS)

    if arguments.pins_only:
        print(
            f"Spec pin consistency OK ({matched} pins matched in "
            f"{AUTHORITY_DOCUMENT.as_posix()}; self-test {controls}/{controls} controls)."
        )
        print(
            f"SPEC BUNDLE: NOT VERIFIED (--pins-only; the v{FROZEN_BUNDLE.version} bundle at "
            f"{declared_location} was not read)"
        )
        return 0

    bundle_directory = (arguments.bundle or Path(declared_location)).resolve()
    if not bundle_directory.is_dir():
        print("Spec bundle check could not run:", file=sys.stderr)
        print(f"  bundle directory does not exist: {bundle_directory}", file=sys.stderr)
        print(
            "  pass --bundle <directory>, or --pins-only to check this repository's "
            "pins alone and record the bundle as unverified",
            file=sys.stderr,
        )
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
        f"Spec bundle check OK ({matched} pins matched in {AUTHORITY_DOCUMENT.as_posix()}; "
        f"{report.file_count} files, {report.byte_count} bytes hashed; "
        f"self-test {controls}/{controls} controls)."
    )
    if report.unlisted:
        print(
            f"  outside the pinned bundle, not checked: {', '.join(report.unlisted)}"
        )
    print(
        f"SPEC BUNDLE: VERIFIED (v{FROZEN_BUNDLE.version} root "
        f"{FROZEN_BUNDLE.pins[0].digest}, {bundle_directory})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
