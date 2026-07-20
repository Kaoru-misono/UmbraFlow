# CLAUDE.md

This repository is a reusable C++23 project foundation. Read
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) before changing module boundaries
and [`docs/INDEX.md`](docs/INDEX.md) for project documentation.


## C++ rules

Use the `cpp-coding` skill for every C++ change. April2 conventions are
authoritative:

- C++23, textual `.hpp`/`.cpp` units, English-only code and comments.
- `camelCase` locals, `m_camelCase` members, `PascalCase` types, kebab-case filenames.
- Trailing return types, east const, AAA locals, Allman braces.
- Follow April2's delimiter-based wrapping exactly: start wrapped content after
  `(` or `{`, indent it, and put the matching delimiter on its own aligned line.
- Source files use LF, four-space indentation with no tabs, no trailing
  whitespace, and exactly one final newline.
- Use `std::byte` for untyped binary storage and `emplace_back` for every
  `std::vector` append operation.
- Recoverable failures use `Result<T>`/`Status`; debug invariants use
  `UF_ASSERT`; release-active invariants use `UF_CHECK`;
  impossible paths use `UF_UNREACHABLE`.
- Return every recoverable failure with the unified `fail(...)` helper. Use
  `UF_TRY*` or normal `std::expected` operations to propagate it, and
  mark result-returning functions `[[nodiscard]]`.
- Do not use `Result<T>` for ordinary absence, expected lookup misses, normal
  control flow, or per-frame hot-path signaling.
- Log a propagated failure once at the application or subsystem boundary.
- Never use direct `std::unreachable`, detached threads, or raw allocation in
  normal project sources.
- Keep casts and raw memory operations under an `unsafe/`, `platform/`, or
  `ffi/` boundary and justify each operation with a nearby `// SAFETY:` comment.

## Ownership

- Use values by default.
- Use `std::unique_ptr<T>` for exclusive ownership.
- Prefer `std::shared_ptr<T const>` for shared immutable data. Mutable shared
  ownership is allowed when the API explicitly defines synchronization and
  mutation semantics.
- Use RAII wrappers for operating-system handles, registrations, and resources.
- Keep platform types behind the module that owns them.
- Express ownership through values, members, and function signatures; do not add
  a common base class solely to impose an ownership model.
- Treat `T&` as a required call-scoped borrow and `T*` as an optional,
  non-owning observation of one object. Raw pointers never represent ownership
  or an array.
- Do not store pointer, reference, iterator, `span`, or `string_view` members by
  default. Exceptions require an explicit backing-owner lifetime contract.
- Synchronous non-escaping lambdas may capture references. Stored or asynchronous
  work must capture by value, move, shared ownership, or a `weak_ptr` locked at
  execution time; it must not retain reference captures or a bare `this`.
- Do not store or return `span`, `string_view`, or other views unless the backing
  lifetime is explicit and guaranteed by the API.
- Move owners before deriving observers from their destination. Never keep an
  observer obtained from an owner that is subsequently moved or reset.
- Use a normal constructor when construction cannot fail, a named static factory
  when establishing the type's invariant can fail, and an owning capability's
  factory when creation requires that capability. Avoid two-phase initialization.

## Tests

- Keep tests as small as possible while covering the public behavior, important
  boundaries, invariants, regressions, and compatibility contracts.
- Temporary diagnostic, characterization, or implementation-scaffolding tests
  are allowed while developing a feature, but remove them before the feature is
  considered complete.
- Retain a test only when it protects behavior worth preserving. Do not retain
  tautologies, trivial getter tests, duplicated cases, or private implementation
  details merely to increase test count or coverage percentage.
- Prefer table-driven boundary cases and one focused test per behavior. Tests
  must remain deterministic and offline.

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

- Never delete, revert, overwrite, or discard unfamiliar files or uncommitted work without explicit approval.
- Never create a worktree without explicit approval.
- Never use destructive Git operations or force-push.
- Stage explicit paths only; never use `git add .` or `git add -A`.
- Never commit without user approval.
- Never add `Co-Authored-By` trailers.
- Keep commits semantic and reviewable.
