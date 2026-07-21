---
name: cpp-coding
description: Use whenever writing, editing, reviewing, or refactoring project C++ code.
---

# C++23 Coding

Read `references/coding-standard.md` completely before changing C++ code. Then consult `core-reuse.md`, `error-handling.md`, and `logging-and-asserts.md` when the change touches those concerns.

## Mandatory rules

1. Target C++23 with textual `.hpp` and `.cpp` files.
2. Reuse the standard library and the existing `core` module before adding helpers or dependencies.
3. Use `Result<T>` or `Status` for recoverable failures,
   `UF_ASSERT` for debug-only invariant evidence,
   `UF_CHECK` for mandatory release-active invariants, and
   `UF_UNREACHABLE` for impossible flow.
4. Return recoverable failures with `fail(...)`; propagate them with
   `UF_TRY*` or normal `std::expected` operations. Do not use results
   for ordinary absence or normal control flow.
5. Log propagated failures once at the CLI or subsystem boundary.
6. Use values by default, `std::unique_ptr` for exclusive ownership, and prefer
   `std::shared_ptr<T const>` for shared immutable data. Mutable shared ownership
   requires explicit synchronization and mutation semantics.
7. Treat references as required call-scoped borrows, raw pointers as optional
   non-owning observations of one object, and views as call-scoped by default.
   Stored borrows and returned views require an explicit backing-lifetime
   contract and focused review.
8. Stored or asynchronous work must not capture references or bare `this`;
   capture owned state or lock a `std::weak_ptr` at execution time.
9. Wrap operating-system handles, registrations, and external resources in RAII types.
10. Keep headers minimal and use forward declarations where ownership permits.
11. All code and comments must be English.
12. Follow the April2 line-wrapping and source-normalization rules in
   `references/coding-standard.md` exactly. Do not introduce a local wrapping
   style.
13. Declare every enum as `enum class` or `enum struct` with an explicit
    project integer underlying type from `<core/types/integer.hpp>`.
14. Use the project aliases for every fixed-width, pointer-width, or
    maximum-width integer instead of spelling the corresponding `std::*_t`
    names. Include `<core/types/integer.hpp>` directly in every file that uses
    them; do not depend on transitive or compiler-injected inclusion.

## Checklist

- Valid C++23 and project formatting
- Platform types stay behind their owning module
- Ownership transfer and borrow lifetime are explicit in every changed API
- Stored views, callbacks, and shared mutable state have enforceable lifetime semantics
- Correct failure mechanism
- April2 delimiter-based wrapping and LF normalization
- Tests are concise, behavior-focused, and cover important boundaries
- Temporary implementation-only tests are removed before completion
