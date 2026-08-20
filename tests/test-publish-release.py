#!/usr/bin/env python3
"""Verify scripts/publish_release.py: shape, determinism, refusals.

The publisher is the only writer of the release manifest, so its output is the
contract ``project init`` parses. This test pins the parts that
matter across a download: the exact member set and canonical spelling, the
release id as a function of the manifest bytes on disk, and the fail-closed
refusals.
"""

from __future__ import annotations

import argparse
import contextlib
import hashlib
import importlib.util
import io
import json
import re
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PUBLISHER = ROOT / "scripts" / "publish_release.py"


def load_publisher():
    spec = importlib.util.spec_from_file_location("publish_release", PUBLISHER)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


def run_main(module, argv: list[str]) -> tuple[int, str, str]:
    """Invoke the publisher's main with a substituted argument vector."""
    previous = sys.argv
    sys.argv = argv
    stdout = io.StringIO()
    stderr = io.StringIO()
    try:
        with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
            code = module.main()
    except SystemExit as exit_error:
        code = exit_error.code if isinstance(exit_error.code, int) else 1
    finally:
        sys.argv = previous
    return code, stdout.getvalue(), stderr.getvalue()


def make_bin_dir(root: Path, platform_name: str) -> Path:
    bin_dir = root / f"bin-{platform_name}"
    bin_dir.mkdir()
    suffix = ".exe" if platform_name == "windows" else ""
    for name in ("project", "umbra-flow", "umbra-flow-conformance"):
        (bin_dir / f"{name}{suffix}").write_bytes(
            f"fake {name} bytes for {platform_name}".encode("utf-8")
        )
    # The runtime payload: the DLLs beside the executables and the OCR models
    # under models/, the layout a fetched umbra-flow answers `--ocr-models
    # <bin>/models` with.
    if platform_name == "windows":
        (bin_dir / "onnxruntime.dll").write_bytes(b"fake onnxruntime dll")
        (bin_dir / "onnxruntime_providers_shared.dll").write_bytes(
            b"fake providers dll"
        )
    for model in ("ppocr-v6-small-rec", "ppocr-v6-small-det"):
        directory = bin_dir / "models" / model
        directory.mkdir(parents=True)
        (directory / "inference.onnx").write_bytes(
            f"fake {model} onnx".encode("utf-8")
        )
        (directory / "inference.yml").write_bytes(
            f"fake {model} yml".encode("utf-8")
        )
    return bin_dir


def check(errors: list[str], condition: bool, message: str) -> None:
    if not condition:
        errors.append(message)


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--root",
        type=Path,
        default=ROOT,
        help="repository root",
    )
    arguments = argument_parser.parse_args()
    if arguments.root.resolve() != ROOT.resolve():
        raise SystemExit("this test must run against its own repository")

    publisher = load_publisher()
    errors: list[str] = []

    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)

        for platform_name in ("windows", "linux", "macos"):
            bin_dir = make_bin_dir(root, platform_name)
            manifest = publisher.build_manifest(
                bin_dir,
                "m0-test",
                platform_name,
                "x64",
                publisher.RELEASE_BINARIES,
            )

            check(
                errors,
                tuple(manifest) == publisher.RELEASE_MANIFEST_MEMBERS,
                f"{platform_name}: manifest members differ from the declared tuple",
            )
            check(
                errors,
                manifest["schema"] == publisher.RELEASE_MANIFEST_SCHEMA,
                f"{platform_name}: manifest schema tag is not the declared one",
            )
            check(
                errors,
                manifest["contract_versions"]
                == list(publisher.RELEASE_MANIFEST_CONTRACT_VERSIONS),
                f"{platform_name}: contract versions differ from the declared tuple",
            )

            rows = manifest["artifacts"]
            check(
                errors,
                [row["name"] for row in rows[:3]]
                == list(publisher.RELEASE_BINARIES),
                f"{platform_name}: binary names differ from the shipped set",
            )
            suffix = ".exe" if platform_name == "windows" else ""
            for row in rows:
                check(
                    errors,
                    "\\" not in row["path"]
                    and not row["path"].startswith("/")
                    and ".." not in row["path"].split("/")
                    and row["path"],
                    f"{platform_name}: artifact path violates the '/' discipline",
                )
                check(
                    errors,
                    row["asset"] == str(row["path"]).replace("/", "-"),
                    f"{platform_name}: asset name does not flatten the path",
                )
                check(
                    errors,
                    "/" not in row["asset"],
                    f"{platform_name}: asset name must be flat for GitHub Releases",
                )
                check(
                    errors,
                    re.fullmatch(r"[0-9a-f]{64}", row["sha256"]) is not None,
                    f"{platform_name}: sha256 is not bare lowercase hex",
                )
                expected = hashlib.sha256(
                    (bin_dir / row["path"]).read_bytes()
                ).hexdigest()
                check(
                    errors,
                    row["sha256"] == expected,
                    f"{platform_name}: sha256 does not match the artifact bytes",
                )
            check(
                errors,
                any(
                    row["path"] == "onnxruntime.dll"
                    for row in rows
                )
                == (platform_name == "windows"),
                f"{platform_name}: the onnxruntime payload row is wrong",
            )
            check(
                errors,
                any(
                    row["path"]
                    == "models/ppocr-v6-small-rec/inference.onnx"
                    for row in rows
                ),
                f"{platform_name}: the OCR model payload row is missing",
            )
            check(
                errors,
                all(
                    row["path"] == row["name"] + suffix
                    for row in rows[:3]
                ),
                f"{platform_name}: binary rows must name their release-root path",
            )

            # The manifest is deterministic: the same inputs produce the same
            # canonical bytes, and therefore the same release id.
            again = publisher.build_manifest(
                bin_dir,
                "m0-test",
                platform_name,
                "x64",
                publisher.RELEASE_BINARIES,
            )
            check(
                errors,
                publisher.canonical_json(manifest)
                == publisher.canonical_json(again),
                f"{platform_name}: publishing twice is not deterministic",
            )

            # End to end: main() writes the manifest and the printed release id
            # is the sha256 of the exact bytes on disk, with no trailing
            # newline.
            output = root / f"release-{platform_name}.json"
            code, stdout, stderr = run_main(
                publisher,
                [
                    "publish_release.py",
                    "--bin-dir",
                    str(bin_dir),
                    "--release",
                    "m0-test",
                    "--output",
                    str(output),
                    "--platform",
                    platform_name,
                    "--arch",
                    "x64",
                ],
            )
            check(errors, code == 0, f"{platform_name}: publisher main failed")
            written = output.read_bytes()
            check(
                errors,
                not written.endswith(b"\n"),
                f"{platform_name}: manifest has a trailing newline",
            )
            match = re.search(r"release id: sha256:([0-9a-f]{64})", stdout)
            check(
                errors,
                match is not None,
                f"{platform_name}: publisher did not print a release id",
            )
            if match is not None:
                check(
                    errors,
                    match.group(1) == hashlib.sha256(written).hexdigest(),
                    f"{platform_name}: release id is not the sha256 of the "
                    "manifest bytes",
                )
            parsed = json.loads(written)
            check(
                errors,
                parsed == manifest,
                f"{platform_name}: written manifest differs from the built one",
            )

            # A declared artifact that is not on disk is refused by name.
            empty = root / f"empty-{platform_name}"
            empty.mkdir()
            refused = False
            try:
                publisher.artifact_rows(
                    empty,
                    publisher.RELEASE_BINARIES,
                    platform_name,
                    "x64",
                )
            except SystemExit as refusal:
                refused = "release artifact is missing" in str(refusal)
            check(
                errors,
                refused,
                f"{platform_name}: a missing artifact was not refused",
            )

        # A bin directory that does not exist is refused at the CLI.
        code, _stdout, _stderr = run_main(
            publisher,
            [
                "publish_release.py",
                "--bin-dir",
                str(root / "does-not-exist"),
                "--release",
                "m0-test",
            ],
        )
        check(
            errors,
            code != 0,
            "a missing bin directory was not refused at the CLI",
        )

    if errors:
        for error in errors:
            print(f"publish_release violation: {error}", file=sys.stderr)
        return 1

    print("publish_release: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
