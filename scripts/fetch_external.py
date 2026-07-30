"""Fetch and verify the third-party payloads modules declare in external manifests.

A payload is identified by its sha256, never by its URL. A download whose digest
does not match the manifest is discarded before anything is written into the
tree, so a mirror, a re-upload, or a silently retagged release fails here instead
of quietly changing what the project builds against.

Re-running is cheap and safe: a payload already present with the right digest is
skipped, and a payload present with the wrong digest is replaced.

    python scripts/fetch_external.py             # fetch everything missing
    python scripts/fetch_external.py --check     # report, download nothing
    python scripts/fetch_external.py --module ocr
"""

from __future__ import annotations

import argparse
import hashlib
import shutil
import sys
import tempfile
import tomllib
import urllib.error
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

# Read in chunks rather than whole: one of these payloads is 79 MB and there is
# no reason for the peak to scale with the largest release anyone ever adds.
CHUNK_BYTES = 1 << 20

SUPPORTED_SCHEMA = "umbraflow-external/v1"


@dataclass(frozen=True)
class Payload:
    module: str
    name: str
    version: str
    url: str
    sha256: str
    destination: Path
    archive: str | None
    platforms: tuple[str, ...]

    # Glob patterns removed from an extracted archive. Debug symbols are the
    # motivating case: they can dwarf the payload and serve nobody who is not
    # debugging the dependency itself.
    prune: tuple[str, ...]

    @property
    def label(self) -> str:
        return f"{self.module}/{self.name}"


def platform_tag() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    return "linux"


def digest_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(CHUNK_BYTES), b""):
            digest.update(block)
    return digest.hexdigest()


def stamp_path(destination: Path) -> Path:
    """Where an extracted payload records the archive digest it came from.

    An archive has no single file in the tree to hash afterwards, and hashing the
    extracted tree instead would make the answer depend on extraction details
    rather than on the bytes the manifest pinned. The stamp records what was
    verified, so a re-run can tell "already fetched from the right archive" from
    "some directory happens to be here".
    """
    return destination.parent / f"{destination.name}.sha256"


def load_manifest(path: Path) -> list[Payload]:
    with path.open("rb") as handle:
        document = tomllib.load(handle)

    schema = document.get("schema")
    if schema != SUPPORTED_SCHEMA:
        raise SystemExit(
            f"{path}: unsupported schema {schema!r}; this script reads {SUPPORTED_SCHEMA!r}"
        )

    module = document.get("module")
    if not module:
        raise SystemExit(f"{path}: manifest declares no module")

    external_root = path.parent
    payloads: list[Payload] = []
    for entry in document.get("payload", []):
        missing = [k for k in ("name", "url", "sha256", "destination") if k not in entry]
        if missing:
            raise SystemExit(f"{path}: payload is missing {', '.join(missing)}")
        payloads.append(
            Payload(
                module=module,
                name=entry["name"],
                version=entry.get("version", ""),
                url=entry["url"],
                sha256=entry["sha256"].lower(),
                destination=external_root / entry["destination"],
                archive=entry.get("archive"),
                platforms=tuple(entry.get("platforms", ())),
                prune=tuple(entry.get("prune", ())),
            )
        )
    return payloads


def is_satisfied(payload: Payload) -> bool:
    if not payload.destination.exists():
        return False
    if payload.archive:
        stamp = stamp_path(payload.destination)
        return stamp.is_file() and stamp.read_text(encoding="utf-8").strip() == payload.sha256
    return digest_of(payload.destination) == payload.sha256


def download(payload: Payload, into: Path) -> Path:
    target = into / "payload.bin"
    try:
        with urllib.request.urlopen(payload.url, timeout=120) as response:
            with target.open("wb") as handle:
                shutil.copyfileobj(response, handle, CHUNK_BYTES)
    except urllib.error.URLError as error:
        raise SystemExit(f"{payload.label}: download failed: {error}") from error
    return target


def place_archive(payload: Payload, downloaded: Path, staging: Path) -> None:
    """Extracts an already-verified archive into its destination.

    A single top-level directory is flattened, because a release zip names that
    directory after its version and the destination already carries the version
    in the manifest.
    """
    if payload.archive != "zip":
        raise SystemExit(f"{payload.label}: unsupported archive kind {payload.archive!r}")

    unpacked = staging / "unpacked"
    with zipfile.ZipFile(downloaded) as archive:
        for member in archive.namelist():
            # Refuse a member that would escape the staging directory. The
            # archive's own digest was verified before this ran, so this cannot
            # trigger on a payload the manifest pinned -- it is what keeps a
            # future manifest edit from turning a bad archive into arbitrary
            # writes across the tree.
            resolved = (unpacked / member).resolve()
            if not resolved.is_relative_to(unpacked.resolve()):
                raise SystemExit(f"{payload.label}: archive member escapes: {member}")
        archive.extractall(unpacked)

    roots = list(unpacked.iterdir())
    source = roots[0] if len(roots) == 1 and roots[0].is_dir() else unpacked

    # Pruned before the move, so the destination never briefly holds bytes the
    # manifest said to drop.
    for pattern in payload.prune:
        for victim in sorted(source.glob(pattern)):
            if victim.is_file():
                victim.unlink()

    payload.destination.parent.mkdir(parents=True, exist_ok=True)
    if payload.destination.exists():
        shutil.rmtree(payload.destination)
    shutil.move(str(source), str(payload.destination))
    # newline="\n" so the stamp is the same bytes on every host: it is compared
    # verbatim on re-run, and Windows text mode would otherwise write CRLF and
    # make a fetched tree differ from the committed one.
    stamp_path(payload.destination).write_text(
        payload.sha256 + "\n", encoding="utf-8", newline="\n"
    )


def fetch(payload: Payload) -> None:
    with tempfile.TemporaryDirectory(prefix="uf-external-") as temporary:
        staging = Path(temporary)
        downloaded = download(payload, staging)

        # Verified before anything is extracted or moved, so a payload that
        # fails leaves the tree exactly as it was. This is the whole reason the
        # manifest pins a digest rather than trusting the URL.
        actual = digest_of(downloaded)
        if actual != payload.sha256:
            raise SystemExit(
                f"{payload.label}: downloaded digest {actual}\n"
                f"  manifest expects {payload.sha256}\n"
                f"  the download was discarded; nothing was written into the tree"
            )

        if payload.archive:
            place_archive(payload, downloaded, staging)
            return

        payload.destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(downloaded), str(payload.destination))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--check",
        action="store_true",
        help="report what is missing or stale and download nothing",
    )
    parser.add_argument(
        "--module",
        help="fetch only this module's payloads",
    )
    arguments = parser.parse_args()

    manifests = sorted(REPOSITORY_ROOT.glob("modules/*/external/manifest.toml"))
    if not manifests:
        print("no external manifests found")
        return 0

    host = platform_tag()
    missing = 0
    for manifest in manifests:
        for payload in load_manifest(manifest):
            if arguments.module and payload.module != arguments.module:
                continue
            if payload.platforms and host not in payload.platforms:
                print(f"skip    {payload.label} (not for {host})")
                continue
            if is_satisfied(payload):
                print(f"present {payload.label}")
                continue

            missing += 1
            if arguments.check:
                print(f"MISSING {payload.label} {payload.version}")
                continue

            print(f"fetch   {payload.label} {payload.version} <- {payload.url}")
            fetch(payload)
            print(f"        -> {payload.destination.relative_to(REPOSITORY_ROOT)}")

    if arguments.check and missing:
        print(
            f"\n{missing} payload(s) missing. Run: python scripts/fetch_external.py",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
