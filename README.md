# UmbraFlow

Native C++ visual automation framework for Windows: WGC capture, template matching, strict background PostMessage input

## Included

- Manifest-driven CMake modules
- MSVC, Linux Clang/GCC, and macOS Clang presets
- Ninja and Visual Studio 2022 workflows
- Optional ccache integration
- Doctest-based unit tests and a local CI entry point
- Warnings-as-errors, compiler hardening, clang-tidy, and sanitizer presets
- Required hosted CI across GCC, Apple Clang, MSVC, pinned Clang analysis, and sanitizers
- Rust-informed C++ capabilities, explicit unsafe boundaries, and module validation
- Checked arithmetic, strong domain types, structured results, and safe contracts
- Checked contiguous access and portable lifetime/thread-safety annotations
- Variant matching, explicit control flow, scope cleanup, type-safe flags, and
  lock-coupled state
- Compile-time-validated enum names with exact bidirectional conversion
- April2 C++23 coding rules, including strict delimiter-based wrapping
- Reusable review, diagnosis, planning, and validation skills
- Deterministic LF source normalization


## Build

Use `python` on Windows and `python3` on Linux or macOS for the Python commands
shown below.

On Windows, activate MSVC in the current shell first:

```bat
call .claude\skills\build-project\script\windows\build-env.bat
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --test-dir build/x64-debug -L CI --output-on-failure
```

Linux and macOS use `linux-debug` and `macos-debug` respectively. See
`.claude/skills/build-project/SKILL.md` for the complete workflow.

Run the repository-level safety checks without compiling:

```bash
python scripts/fix_format.py --check
python scripts/check_cpp_format.py
python scripts/check_modules.py
python scripts/check_safety.py
```

`fix_format.py --check` validates every first-party UTF-8 text file. Run it
without `--check` to normalize line endings, trailing whitespace, C++ tabs, and
the final newline. `check_cpp_format.py` checks the member and assignment
alignment rules on first-party C++ under `modules`, `entry`, and `tests`,
excluding vendored code; pass `--fix` to apply them. It uses only the standard
library and deliberately leaves line wrapping to the April2 convention, which
is a judgment rule no formatter can express. `bash scripts/ci-local.sh` selects
the host debug preset and Python 3 interpreter automatically.

AddressSanitizer and static-analysis presets are available as `x64-asan` and
`x64-analysis`. Linux additionally provides `linux-ubsan` and `linux-tsan`.
The complete safety contract is documented in
[`cpp-coding/references/safety-profile.md`](.claude/skills/cpp-coding/references/safety-profile.md).
