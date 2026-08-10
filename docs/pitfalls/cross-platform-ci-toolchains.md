# Cross-platform CI toolchain diagnostics

The strict CI matrix deliberately combines compiler warnings, custom repository
gates, Clang lifetime analysis, and clang-tidy. A change that passes the native
Windows debug build can therefore still expose a portability or analysis issue
on another CI job. Keep the checks strict and fix the boundary that produced the
diagnostic.

## MSVC treats external-header warnings as project errors

### Symptom

Windows MSVC fails under `/W4 /WX` because a header supplied by the toolchain or
through the `INCLUDE` environment variable emits a warning. The same source may
build on GCC and Clang, and changing project code does not remove the warning.

### Root cause

Without an external-header policy, MSVC applies the project's warning level to
headers discovered through `INCLUDE`. `/WX` then promotes those warnings to
errors even though the project does not own the header.

### Fix

Keep `/W4` and `/WX` for project code, but configure environment-provided headers
as external in `cmake/warnings.cmake`:

```cmake
/external:env:INCLUDE
/external:W0
```

Do not mark project include directories as external and do not globally lower
the warning level.

### Regression check

Run both Windows MSVC Debug and Release CI jobs. A deliberately introduced
warning in project code must still fail the build, while warnings originating in
environment-provided external headers must not.

## Place Clang lifetime attributes after `noexcept`

### Symptom

Clang rejects a trailing-return declaration that places `UF_LIFETIME_BOUND`
before `noexcept`, even though the macro expands to nothing on other compilers.

### Root cause

On Clang, `UF_LIFETIME_BOUND` expands to `[[clang::lifetimebound]]`. For a
qualified member declaration using a trailing return type, the accepted order is
the function qualifiers, `noexcept`, the lifetime attribute, and then `->`.

### Fix

Use this order consistently:

```cpp
auto value() const noexcept UF_LIFETIME_BOUND -> Value const&;
```

Do not use:

```cpp
auto value() const UF_LIFETIME_BOUND noexcept -> Value const&;
```

### Regression check

Run the macOS Clang job and the pinned Clang lifetime-analysis job. Also run
`scripts/check_safety.py`, whose declaration matcher must recognize the accepted
ordering.

## A `friend` declaration can share `[[nodiscard]]` with its free declaration

### Symptom

`scripts/check_safety.py` reports that a `friend` function returning `Result`,
`Status`, or `std::optional` is missing `[[nodiscard]]`, even though the matching
namespace-scope declaration is annotated.

### Root cause

A friend declaration inside a class can redeclare the same free function. A
purely declaration-by-declaration text check cannot tell that the must-use
contract is already present elsewhere and produces a false positive.

### Fix

The safety check associates declarations by function name and normalized
parameter list. It permits an unannotated `friend` redeclaration only when the
same header contains a matching annotated declaration. A different overload
does not satisfy the requirement.

Do not add redundant attributes merely to satisfy the text scanner, and do not
exempt all friend functions from the rule.

### Regression check

Keep focused tests for all three cases in `tests/test-check-safety.py`:

- A matching annotated free declaration covers its friend redeclaration.
- A genuinely unannotated must-use function is reported.
- An annotated overload does not cover a friend with different parameters.

Run both `python scripts/check_safety.py` and
`python -m unittest tests/test-check-safety.py` on Windows, using `python3` on
Linux and macOS.

## Clang-tidy does not understand doctest `REQUIRE` as an optional guard

### Symptom

Clang 23 reports `bugprone-unchecked-optional-access` for `*optional` or
`optional.value()` immediately after a doctest `REQUIRE(optional.has_value())`.
The test is safe at runtime, but the analysis job still fails because warnings
are errors.

### Root cause

The clang-tidy check requires a value-presence guarantee that is visible in the
local control-flow graph. It recognizes only selected assertion macros and does
not model doctest's `REQUIRE`. By default, it treats `value()`, `operator*`, and
`operator->` as equivalent optional accesses, so replacing `*optional` with
`optional.value()` does not fix the diagnostic.

### Fix

When a helper must extract the value, make the guard visible to the analyzer:

```cpp
if (!kind.has_value())
{
    FAIL("The error did not contain an automation error kind");
    return AutomationErrorKind::InternalInvariant;
}
return *kind;
```

When extraction is unnecessary, compare the optional as a whole. For nested
`Result<std::optional<T>>`, first verify the `Result`, then compare its contained
optional without dereferencing that optional:

```cpp
REQUIRE(result.has_value());
CHECK(*result == std::optional{expected});
```

Do not disable `bugprone-unchecked-optional-access`, add a blanket `NOLINT`, or
use `value()` as a cosmetic workaround.

### Regression check

Run the pinned Clang lifetime, bounds, thread-safety, and tidy CI job. Ensure its
analysis build and tests both complete, because parallel compilation may stop at
the first few optional diagnostics and hide later occurrences.

## doctest includes `<ciso646>` under Clang, which libstdc++ 15 warns about

This one is not a current failure. It is written down so that whoever meets it
recognises it in a minute instead of a day.

### Symptom

Roughly two dozen doctest translation units stop compiling at once, under Clang
only, with a `#warning` from `<ciso646>` promoted to an error by `-Werror`. The
same commit builds under GCC, and built under Clang on the previous runner
image. No project source mentions `<ciso646>`.

### Root cause

`tests/external/doctest/doctest/doctest.h` (2.4.11) does this at line 498:

```cpp
#if DOCTEST_CLANG
#include <ciso646>
#endif // clang
```

It is deliberate — doctest wants `_LIBCPP_VERSION` before deciding whether to
forward-declare standard types. `<ciso646>` was deprecated in C++17 and removed
in C++20, and libstdc++ 15 added a `#warning` to its remaining copy. Under
`-Werror` that warning is fatal, and it lands in every TU that includes
doctest.

The include is guarded on `DOCTEST_CLANG`, so GCC never reaches it regardless of
which libstdc++ is installed. Clang plus libstdc++ 15 is the only combination
that fails. libstdc++ 13 and 14 carry no such warning, and GitHub's
`ubuntu-latest` is 24.04 with libstdc++ 14 — which is the whole reason CI is
green today.

### Trigger condition

The moment `ubuntu-latest` rolls forward to an image whose default libstdc++ is
15 or newer, every Clang job that compiles doctest breaks at once, with no
project change. A local Clang build on a newer distribution hits it earlier.

### Fix

Do not silence `-Werror` for the test targets. The include already sits in
vendored third-party code, which the build treats as a system include
elsewhere; reaching doctest through `SYSTEM` include directories is what
suppresses the warning without weakening the project's own diagnostics.
Upgrading doctest past the release that drops the include also removes it.

### Regression check

Build one doctest translation unit with Clang against a libstdc++ 15 or newer
toolchain and `-Werror`. Before the fix it fails on `<ciso646>`; after it, it
compiles, and a deliberate warning in project code must still fail the build.
