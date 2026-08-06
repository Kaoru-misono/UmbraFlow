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

Run that form from PowerShell, not from Git Bash, which rewrites the `/c`
argument into a path and opens an interactive shell instead.

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

When a source file is added or renamed, rerun
`cmake --preset <host-debug-preset>` before building because module sources are
discovered during configuration.

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
