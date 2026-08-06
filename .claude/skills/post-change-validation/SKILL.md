---
name: post-change-validation
description: Run the repository gates — formatting, module and safety checks, focused build targets, and tests. Use after every code or build-system change, and before reporting work complete.
---

# Post-change validation

Use `python` on Windows and `python3` on Linux or macOS in the commands below.

1. Review the diff and identify affected modules. Classify new tests as permanent
   contract/regression coverage or temporary implementation scaffolding; remove
   the temporary tests before completion.
2. Run `python scripts/fix_format.py --check`.
3. Run `python scripts/check_cpp_format.py`.
4. Run `python scripts/check_modules.py`.
5. Run `python scripts/check_safety.py`.
6. Select the host preset (`x64-debug`, `linux-debug`, or `macos-debug`) and
   configure after adding or renaming source files.
7. Build the smallest affected target, then the full tree.
8. Run `ctest --test-dir build/<host-preset> -L CI --output-on-failure`.
9. Run any project-specific integration or hardware checks required by the changed subsystem.
10. Report every command and its exit status. Do not call a change verified when a required toolchain or environment was unavailable.

Read `references/test-patterns.md` when adding tests.
