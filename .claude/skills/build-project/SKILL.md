---
name: build-project
description: Configure, build, and test the C++ project with its CMake presets. Use for every compile, rebuild, or build failure investigation.
---

# Build the project

The command exit code is authoritative.

## Prerequisites

- CMake 3.30 or newer
- Ninja
- Windows: Visual Studio 2022 Desktop development with C++ and a Windows SDK
- Linux/macOS: a C++23 compiler available to CMake

Activate MSVC in the same process that invokes CMake. `vcvars64` must run inside
the process that later spawns `cl.exe`, so from a cmd session:

```cmd
call .claude\skills\build-project\script\windows\build-env.bat
cmake --preset x64-debug
cmake --build --preset x64-debug
```

From PowerShell, put the activation and the command in one cmd process; the
child command inherits the activated environment:

```powershell
cmd /c "call .claude\skills\build-project\script\windows\build-env.bat && cmake --build --preset x64-debug"
```

From Git Bash or the Bash tool, double the switch. MSYS path conversion rewrites
a lone `/c` into a path, so `cmd.exe` never gets a switch: it prints its banner,
runs nothing, and exits 0, which looks like success:

```bash
cmd //c "call .claude\skills\build-project\script\windows\build-env.bat && cmake --build --preset x64-debug"
```

`MSYS_NO_PATHCONV=1` in front of the single-`/c` form does the same. Probe an
unfamiliar shell with `cmd /c "echo probe"` against `cmd //c "echo probe"`; only
the converting shell swallows the first. Verified 2026-08-10 —
`docs/pitfalls/repository-tooling-invocation.md`.

Select the preset for the current host:

| Host | Debug | Release | Analysis |
|---|---|---|---|
| Windows | `x64-debug` | `x64-release` | `x64-analysis` |
| Linux | `linux-debug` | `linux-release` | `linux-analysis` |
| macOS | `macos-debug` | `macos-release` | `macos-analysis` |

AddressSanitizer presets use the same host prefix. Linux and macOS also provide
UBSan presets; Linux provides TSan. Visual Studio build presets are
`vs2022-debug` and `vs2022-release`.

Useful target patterns:

```bash
cmake --build --preset <host-debug-preset> --target test-core
```

Library targets use `${PROJECT_NAME}_<module>`, and the starter executable target
is `${PROJECT_NAME}`.

Run tests after a successful build:

```bash
ctest --test-dir build/<host-debug-preset> -L CI --output-on-failure
```

Adding or renaming a source needs no manual reconfigure. `cmake/build.cmake`
globs module sources with `CONFIGURE_DEPENDS`, so `cmake --build` re-evaluates
the glob and reconfigures itself when the file set changes; `entry/` and
`tests/` list sources explicitly, and editing their `CMakeLists.txt` triggers
the same reconfigure. Rerun `cmake --preset <host-debug-preset>` only when a
preset, toolchain or cache variable changes.

## The whole gate in one command

`scripts/ci-local.ps1` and `scripts/ci-local.sh` run the four Python checks,
configure, build, and `ctest -L CI` in that order, choose the preset from the
host, stop at the first failure, and print `GATE: PASS` only when every step
passed. Both take an optional target to narrow the build, and `CPP_PRESET`
overrides the preset. On Windows the script still needs an already-activated
MSVC session:

```powershell
cmd /c "call .claude\skills\build-project\script\windows\build-env.bat && pwsh -NoProfile -File scripts\ci-local.ps1"
```

`//c` here too when the shell is Bash-like.
