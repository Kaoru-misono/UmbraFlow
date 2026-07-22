# CLAUDE.md

This repository is a reusable C++23 project foundation. Read
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before changing module boundaries
and [`docs/INDEX.md`](docs/INDEX.md) for project documentation.

## Required skills

- Use `cpp-coding` for every C++ implementation, refactor, or review.
- Use `evaluate-core-capability` before adding or promoting a generic facility
  to `core`.
- Use `manage-git-changes` for staging, committing, reorganizing unpublished
  commits, creating worktrees, or pushing.
- Use `build-project` for every configure, build, or test invocation.
- Use `post-change-validation` after code changes.

## Always-on C++ safety

- Ownership must remain visible in values, members, parameters, and returns.
  Raw pointers never own resources or represent pointer-plus-size buffers.
- Stored borrows and returned views require an explicit backing-owner lifetime
  contract. Stored or asynchronous work must not retain reference captures or a
  bare `this`.
- Never use direct `std::unreachable`, detached threads, or raw allocation in
  normal project sources.
- Keep casts and raw memory operations under an `unsafe/`, `platform/`, or
  `ffi/` boundary and justify each operation with a nearby `// SAFETY:` comment.
- Do not use reference, pointer, view, or callback parameters to return computed
  values. Return one value directly or return multiple values in a named result
  type. Allow a mutable parameter only when mutating caller-owned state is the
  function's primary operation, an external API/ABI contract requires it, or a
  measured hot-path requirement rules out returning the value; justify every
  non-obvious exception at the declaration.

## Build

```bash
cmake --preset <host-debug-preset>
cmake --build --preset <host-debug-preset>
```

On Windows, activate MSVC in the same shell first with
`.claude/skills/build-project/script/windows/build-env.bat`. Linux and macOS
use the `linux-debug` and `macos-debug` presets. A Visual Studio solution can be
generated with `cmake --preset vs2022`.

Useful starter targets are `${PROJECT_NAME}_core`, `${PROJECT_NAME}`, and
`test-core`.

## Verification

After code changes, use the `post-change-validation` skill. The minimum gate is:

Use `python` on Windows and `python3` on Linux or macOS for Python commands.

```bash
python scripts/fix_format.py --check
python scripts/check_modules.py
python scripts/check_safety.py
cmake --build --preset <host-debug-preset>
ctest --test-dir build/<host-debug-preset> -L CI --output-on-failure
```

Use `x64-debug` on Windows, `linux-debug` on Linux, and `macos-debug` on macOS.

## Plans and pitfalls

- Permanent plans: `docs/plans/YYYY-MM-DD-<topic>.md`.
- Reusable failure knowledge: `docs/pitfalls/`.
- Intentional shortcuts: `TODO(cpp-debt): ...` and the `cpp-debt` skill.

## Workflow red lines

- Use the `manage-git-changes` skill for every Git mutation.
- Never delete, revert, overwrite, or discard unfamiliar files or uncommitted work without explicit approval.
- Never create a worktree without explicit approval.
- Never use destructive Git operations or force-push.
- Stage explicit paths only; never use `git add .` or `git add -A`.
- Never commit without user approval.
- Never add `Co-Authored-By` trailers.
- Keep commits semantic and reviewable.
