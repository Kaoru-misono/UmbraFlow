---
name: cpp-coding
description: Apply the project's C++23 coding, ownership, safety, error-handling, and formatting rules. Use whenever writing, editing, reviewing, or refactoring project C++ headers or sources under modules/, entry/, or tests/. Do not use for CMake, Python, or Markdown-only changes.
---

# C++23 Coding

Read `references/coding-standard.md` completely before changing C++ code. Read
`references/safety-profile.md` when a change touches APIs, ownership, lifetimes,
concurrency, numeric safety, serialization, or an `unsafe/`, `platform/`, or
`ffi/` boundary. Consult `core-reuse.md`, `error-handling.md`, and
`logging-and-asserts.md` when the change touches those concerns.

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
10. Give every stored data member an in-class brace initializer, so that
    `Widget value;` leaves nothing indeterminate. Omit it only when the value
    must come from construction, and then brace-initialize the member in every
    constructor's member-initializer list. Never both, when every constructor
    initializes it: the constructor wins, so the in-class initializer would be
    dead. An initializer only some constructors override is correct and stays.
11. Classify every alias before writing it by asking whether code outside the
    translation unit can name it. If it can — namespace scope in a header, or a
    public or protected member of a type reachable from one — it is vocabulary
    and must add meaning the expansion lacks. If it cannot — private,
    function-local, or in an anonymous namespace in a `.cpp` — it is a local
    abbreviation and must earn its scope through repetition or alignment.
    Neither may hide optionality, borrow scope, container shape, or an
    ownership category.
12. Keep headers minimal and use forward declarations where ownership permits.
13. All code and comments must be English.
14. Follow the repository line-wrapping and source-normalization rules in
    `references/coding-standard.md` exactly. Do not introduce a local wrapping
    style.
15. Declare every enum as `enum class` or `enum struct` with an explicit
    project integer underlying type from `<core/types/integer.hpp>`.
16. Use the project aliases for every fixed-width, pointer-width, or
    maximum-width integer instead of spelling the corresponding `std::*_t`
    names. Include `<core/types/integer.hpp>` directly in every file that uses
    them; do not depend on transitive or compiler-injected inclusion.
17. Nest every file-local anonymous namespace inside the narrowest owning `uf`
    namespace. Never put an anonymous namespace directly at global scope.
18. Do not use parameters as output channels. Return one value directly or
    return multiple values in a named result type. Mutable parameters are
    allowed only when mutating caller-owned state is the function's primary
    operation, an external API/ABI contract requires it, or a measured hot-path
    requirement rules out returning the value; justify every non-obvious
    exception at the declaration.
19. Ask whether an operation needs a type before writing its signature, following
    "Whether a type exists" in `references/coding-standard.md`. Ask then, because
    afterwards nothing asks. A free function is usually right; three or more free
    functions sharing a first parameter this module owns and operates on are a
    receiver passed by hand.
20. Dispatch over a closed set -- variant alternatives, enumerators, a fixed
    command vocabulary -- as a table or an overload set, never as a chain of
    `if`/`else if`. A closed set has a known size, so a chain over it accepts a new
    member with no branch and the compiler cannot say so.

## Checklist

Run the repository gates for everything they cover, and do not re-verify those
by reading:

```bash
python scripts/fix_format.py --check
python scripts/check_cpp_format.py
python scripts/check_modules.py
python scripts/check_safety.py
```

Between them they enforce byte-level normalization, the module dependency graph,
`[[nodiscard]]` on `Result`/`Status`/`optional`, and the unsafe-boundary rules
in full. They enforce alignment and data member initialization only in part:
both are conservative recognizers that stay silent on declarations they cannot
parse, so their residue is on the list below. The required `clang-analysis` CI
job adds clang-tidy lifetime, bounds, and member-init checks that do not run on
Windows.

Nothing checks the items below. They are the ones that reach review unnoticed,
so verify each one deliberately:

- Repository delimiter-based wrapping on every wrapped statement, not only calls
- Trailing return types, Allman braces, AAA locals, east const, and braces on
  control statements
- Where an alignment block boundary belongs, and any declaration form the
  alignment recognizer skips
- Each new alias classified by external reachability, and justified under that
  category
- Whether a type exists at all, asked before the signature was written: no third
  free function sharing a first parameter this module owns and operates on
- No `if`/`else if` chain dispatching over a closed set
- Each new header-level type placed by ownership: nested in its class when
  nothing names it except that class's operations, at namespace scope when it
  has independent consumers or belongs to a free-function layer
- Every member without an in-class initializer supplied explicitly at each
  aggregate construction site, with no invented sentinel
- Ownership transfer and borrow lifetime explicit in every changed API
- Description parameters by `T const&`, normalized into a named local rather
  than mutated in place, with forwarding references only at generic boundaries
- No output parameters unless a permitted exception is explicit and justified
- Stored views, callbacks, and shared mutable state have enforceable lifetime
  semantics
- Correct failure mechanism, with failures logged once at a boundary
- Scoped enums carrying an explicit project integer underlying type
- Project integer aliases, with `<core/types/integer.hpp>` included directly
- Anonymous namespaces nested inside the owning `uf` namespace
- Include order, and class body order with stored state not interleaved with
  methods
- Platform types stay behind their owning module
- Tests concise and behavior-focused; temporary tests removed before completion
