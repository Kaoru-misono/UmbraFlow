from pathlib import Path
import subprocess
import sys
import tempfile
import time


CASE = "fault matrix crash recovers a sent unrecorded mutation"


def main() -> int:
    executable = Path(sys.argv[1])
    base = Path(sys.argv[2])
    base.mkdir(parents=True, exist_ok=True)
    root = Path(tempfile.mkdtemp(prefix="crash-", dir=base))

    (root / "fault-root").write_text("fault matrix crash\n", encoding="utf-8")
    (root / "child-mode").write_text("child\n", encoding="utf-8")
    child = subprocess.Popen(
        [str(executable), f"--test-case={CASE}"],
        cwd=root,
    )
    deadline = time.monotonic() + 20.0
    signal = root / "action-sent"
    while not signal.is_file() and child.poll() is None:
        if time.monotonic() >= deadline:
            child.kill()
            child.wait()
            print("crash child did not signal the durable target state", file=sys.stderr)
            return 1
        time.sleep(0.05)

    if child.poll() is not None:
        print(f"crash child exited before the kill point: {child.returncode}", file=sys.stderr)
        return 1

    child.kill()
    child.wait()

    (root / "verify-mode").write_text("verify\n", encoding="utf-8")
    verified = subprocess.run(
        [str(executable), f"--test-case={CASE}"],
        cwd=root,
        check=False,
    )
    return verified.returncode


if __name__ == "__main__":
    raise SystemExit(main())
