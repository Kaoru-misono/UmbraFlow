#!/usr/bin/env python3
"""Verify that the pure-VM identity pins the repository's exact Luau gitlink."""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
LUAU_GITLINK = "modules/script/external/luau"
IDENTITY_SOURCE = REPOSITORY_ROOT / "modules/script/source/script/ffi/pure-data-program.cpp"
IDENTITY_PATTERN = re.compile(
    r"k_luauImplementation\s*=\s*std::string_view\s*\{\s*"
    r'"luau-[^"]+\+([0-9a-f]{40})"\s*\}'
)


def git_revision(*arguments: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip()
        raise RuntimeError(f"git {' '.join(arguments)} failed: {detail}")
    return completed.stdout.strip()


def main() -> int:
    source = IDENTITY_SOURCE.read_text(encoding="utf-8")
    revisions = IDENTITY_PATTERN.findall(source)
    if len(revisions) != 1:
        print(
            f"ERROR: expected exactly one Luau revision in {IDENTITY_SOURCE}",
            file=sys.stderr,
        )
        return 1

    try:
        gitlink_revision = git_revision("rev-parse", f":{LUAU_GITLINK}")
        checkout_revision = git_revision("-C", LUAU_GITLINK, "rev-parse", "HEAD")
    except RuntimeError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    identity_revision = revisions[0]
    failures: list[str] = []
    if identity_revision != gitlink_revision:
        failures.append(
            "pure VM identity revision "
            f"{identity_revision} != gitlink revision {gitlink_revision}"
        )
    if checkout_revision != gitlink_revision:
        failures.append(
            f"Luau checkout revision {checkout_revision} != gitlink revision {gitlink_revision}"
        )

    if failures:
        for failure in failures:
            print(f"ERROR: {failure}", file=sys.stderr)
        return 1

    print(f"Luau environment identity: OK ({gitlink_revision})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
