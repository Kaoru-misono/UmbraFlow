---
name: post-change-validation
description: Validate project changes with formatting, focused build targets, tests, and static inspection.
---

# Post-change validation

Use `python` on Windows and `python3` on Linux or macOS in the commands below.

1. Review the diff and identify affected modules. Classify new tests as permanent
   contract/regression coverage or temporary implementation scaffolding; remove
   the temporary tests before completion.
2. Run `python scripts/fix_format.py --check`.
3. Run `python scripts/check_modules.py`.
4. Run `python scripts/check_safety.py`.
5. Select the host preset (`x64-debug`, `linux-debug`, or `macos-debug`) and
   configure after adding or renaming source files.
6. Build the smallest affected target, then the full tree.
7. Run `ctest --test-dir build/<host-preset> -L CI --output-on-failure`.
8. Run any project-specific integration or hardware checks required by the changed subsystem.
9. Report every command and its exit status. Do not call a change verified when a required toolchain or environment was unavailable.

When changing template identity or initialization behavior, also run
`python scripts/initialize_project.py SampleProject --dry-run` and exercise the
initializer in a disposable copy of the repository.

Read `references/test-patterns.md` when adding tests.
