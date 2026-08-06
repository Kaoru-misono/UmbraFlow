---
name: post-change-validation
description: Run the repository gates — formatting, module and safety checks, focused build targets, and tests. Use after every code or build-system change, and before reporting work complete.
---

# Post-change validation

Use `python` on Windows and `python3` on Linux or macOS in the commands below.

1. Review the diff and identify affected modules. Classify new tests as permanent
   contract/regression coverage or temporary implementation scaffolding; remove
   the temporary tests before completion.
2. Run the whole gate with `scripts/ci-local.ps1` on Windows or
   `scripts/ci-local.sh` on Linux and macOS. It runs the four checks in step 3,
   configures, builds, and runs `ctest -L CI` in that order, chooses the preset
   from the host, stops at the first failure, and prints `GATE: PASS` only when
   every step passed. Pass a target name to narrow the build while iterating;
   the run that decides the change built the full tree. On Windows it needs an
   MSVC-activated session — see `build-project`.
3. Run a single check directly only while iterating on one failure:
   `python scripts/fix_format.py --check`, `python scripts/check_cpp_format.py`,
   `python scripts/check_modules.py`, `python scripts/check_safety.py`. Passing
   one of them is not validation; only the aggregate script end to end is.
4. Run any project-specific integration or hardware checks required by the changed subsystem.
5. Report every command and its exit status. Do not call a change verified when a required toolchain or environment was unavailable.

Read `references/test-patterns.md` when adding tests.
