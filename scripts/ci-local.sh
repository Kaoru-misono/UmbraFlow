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

ctest --test-dir "build/$PRESET" -L CI --output-on-failure

echo "GATE: PASS"
