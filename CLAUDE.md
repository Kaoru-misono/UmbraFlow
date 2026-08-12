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
- Use `correct-doc-drift` immediately when a decision is finalized or changed,
  and whenever a document contradicts the code or a newer document.
- Use `pitfall-lookup` before investigating any failure, and record a new
  non-obvious root cause there once it is understood.

## Break it rather than bridge it

Nothing here is released and nothing outside this repository consumes it. When
a change needs a different shape, change the shape: rename the field, drop the
old spelling, migrate the data, and fix every caller in the same change.

Do not add a compatibility path to leave existing callers or existing files
untouched — no fallback branch, no version flag, no "absent means the old
behaviour" reading, no accepting two spellings of one thing. A shim outlives
the reason it was added, and the next reader cannot tell which spelling is the
real one.

Where bytes already on disk or already recorded must be understood, migrate
them in the same change and say so, rather than teaching a reader both shapes.
State in the commit message what was broken and what moved with it.

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

After code changes, use the `post-change-validation` skill. The whole gate is one
command: `scripts/ci-local.ps1` on Windows, `scripts/ci-local.sh` on Linux and
macOS. It runs the four checks below, configures, builds, and runs
`ctest -L CI`, chooses the preset from the host, and prints `GATE: PASS` only
when every step passed. On Windows it needs the MSVC activation above.

Run a single check on its own only while iterating on one failure:

```bash
python scripts/fix_format.py --check
python scripts/check_cpp_format.py
python scripts/check_modules.py
python scripts/check_safety.py
```

Use `python` on Windows and `python3` on Linux or macOS. The host presets are
`x64-debug` on Windows, `linux-debug` on Linux, and `macos-debug` on macOS.

## Plans and pitfalls

- Permanent plans: `docs/plans/YYYY-MM-DD-<topic>.md`.
- Reusable failure knowledge: `docs/pitfalls/`.
- Intentional shortcuts: `TODO(cpp-debt): ...`. Harvest them with
  `rg -n "TODO\(cpp-debt\)" --glob '!**/.worktrees/**' modules/ entry/ tests/ cmake/`
  into `docs/plans/cpp-debt-ledger.md`; never edit source while harvesting.
- Archive a finished plan into `docs/archive/plans/` and a closed review into
  `docs/archive/reviews/`. Move the file normally, never with `git mv`, and
  never stage or commit as part of archiving.
- Before archiving, lift what the document still owes into a live document: a
  new one, or an existing one that already owns that kind of work. Give each
  lifted item only enough context to be picked up and acted on; the reasoning
  behind it stays in the archived file and is read on demand. Nothing may be
  archived while something it owes exists only inside it.
- Skills live in `.claude/skills/<name>/SKILL.md`. Only the description is
  loaded every session, so it carries the triggers and nothing else.

## Diagnosing a failure

Reproduce deterministically and confirm the reproduction matches the reported
symptom before forming hypotheses; then test one variable at a time. Tag
temporary instrumentation `[DEBUG-...]` and remove it before completion. When
the user asked for a diagnosis only, do not implement the fix. For a failure
only a human can trigger, adapt a copy of `scripts/hitl-loop.template.sh`.

## Workflow red lines

- Use the `manage-git-changes` skill for every Git mutation.
- Never delete, revert, overwrite, or discard unfamiliar files or uncommitted work without explicit approval.
- Never create a worktree without explicit approval.
- Never use destructive Git operations or force-push.
- Stage explicit paths only; never use `git add .` or `git add -A`.
- Never commit without user approval.
- Never add `Co-Authored-By` trailers.
- Keep commits semantic and reviewable.
