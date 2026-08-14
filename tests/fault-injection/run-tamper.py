from pathlib import Path
import subprocess
import sys


def run(executable: Path, case: str) -> int:
    completed = subprocess.run(
        [str(executable), f"--test-case={case}"],
        check=False,
    )
    return completed.returncode


def main() -> int:
    project = Path(sys.argv[1])
    operator = Path(sys.argv[2])
    release_result = run(
        project,
        "fault matrix tamper names the altered frozen release file",
    )
    if release_result != 0:
        return release_result
    return run(
        operator,
        "fault matrix tamper names the altered signed evidence file",
    )


if __name__ == "__main__":
    raise SystemExit(main())
