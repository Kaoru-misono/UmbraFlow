#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
TARGET="${1:-}"

case "$(uname -s)" in
    Linux*) DEFAULT_PRESET="linux-debug" ;;
    Darwin*) DEFAULT_PRESET="macos-debug" ;;
    CYGWIN*|MINGW*|MSYS*) DEFAULT_PRESET="x64-debug" ;;
    *)
        echo "Unsupported host for automatic preset selection: $(uname -s)" >&2
        exit 2
        ;;
esac

PRESET="${CPP_PRESET:-$DEFAULT_PRESET}"

if command -v python3 >/dev/null 2>&1; then
    PYTHON=python3
elif command -v python >/dev/null 2>&1; then
    PYTHON=python
else
    echo "Python 3 was not found on PATH." >&2
    exit 2
fi

cd "$REPO_ROOT"

"$PYTHON" scripts/fix_format.py --check
"$PYTHON" scripts/check_cpp_format.py
"$PYTHON" scripts/check_modules.py
"$PYTHON" scripts/check_safety.py
"$PYTHON" scripts/check_luau_identity.py
"$PYTHON" scripts/generate_unicode_luau.py --check
"$PYTHON" scripts/generate_public_contract.py --check
cmake --preset "$PRESET"

# A developer works on this machine while the gate runs, so the build is capped
# at half the logical processors rather than Ninja's default of all of them. CI
# invokes `cmake --build --preset ...` directly and never this script, so its
# runners stay uncapped.
BUILD_JOBS=$(( $(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4) / 2 ))
(( BUILD_JOBS < 2 )) && BUILD_JOBS=2

if [[ -n "$TARGET" ]]; then
    cmake --build --preset "$PRESET" --target "$TARGET" -j "$BUILD_JOBS"
else
    cmake --build --preset "$PRESET" -j "$BUILD_JOBS"
fi

# Six, not one and not the whole machine. CTest runs one test at a time by
# default, which spends 362 seconds on a suite whose critical path is its
# slowest single test; six workers finish it in 86. Past roughly six the extra
# processes buy no wall clock and only take the machine away from the developer
# sitting at it. Every gate registered here therefore has to hold its TIMEOUT
# with five siblings running beside it, which is why the two fattest binaries
# are registered as index shards in tests/CMakeLists.txt. The cap lives here
# rather than in CMakePresets.json for the same reason the build cap above
# does: CI invokes ctest directly and stays uncapped.
TEST_JOBS=6

ctest --test-dir "build/$PRESET" -L CI --output-on-failure --parallel "$TEST_JOBS"

echo "GATE: PASS"
