# Checks that cannot fail

A test that stays green after the property it names is removed proves nothing.
The same is true one level up, of the gates themselves: a configured check that
can never report is worse than a missing one, because the missing one is
visible. This repository has now found the failure in both places — in test
bodies, recorded in
[the runtime hardening review](../reviews/2026-08-10-runtime-hardening-review.md),
and in the build's own wiring, below.

The discipline that catches both is the same. Remove the property, watch the
check turn red, put the property back. An untested check is a claim, not a
gate.

## They are one family, not four coincidences

Four separate instances were found on 2026-08-10, in four unrelated files. They
are the same defect wearing four costumes: **a name exists, the name promises
something, and nothing verifies the promise.**

| Instance | The name that promised | What verified it |
|---|---|---|
| `HeaderFilterRegex` in `.clang-tidy` | "headers outside `tests/external` are analyzed" | nothing — the pattern matched no path, so every header diagnostic was dropped as non-user code |
| `SOURCE_ROOTS` in `scripts/check_cpp_format.py` and `scripts/check_safety.py` | "every first-party C++ source is formatted and safety-checked" | nothing — `contract-suite/` was absent from the tuple, so an entire exported module was unscanned |
| `test-annotate-backend` | "the authoring authority is gated" | nothing — the suite existed and ran in no CTest at all |
| `cpp_add_contract_suite` | "these cases run" | nothing — the helper builds the binary `NO_CTEST` and registers one CTest per `CASES` entry, so seven compiled cases in `tests/operator/test-project-plugin-contract.cpp` executed in no gate |

Note what they are *not*. None is a bug in a check's logic; every one of the four
checks works correctly on the inputs it receives. The defect is upstream of the
logic, in what reaches it: an unmatchable filter, a missing root, an unregistered
binary, an un-run case. That is why review does not catch them — reading the
check tells you nothing, because the check is fine.

Two consequences worth carrying:

- **A green result is evidence only when the run states its scope.** "clang-tidy
  passed" means nothing without how many objects it analyzed; "the format check
  passed" means nothing without which roots it walked; "`ctest` passed" means
  nothing without how many of the compiled cases were registered. Any claim of
  the form "X passed" that does not state its denominator is the shape this
  family hides in.
- **Every gate needs its own positive control**, not only every test. Prove the
  filter matches something, the root list reaches the file, the binary is in
  `ctest -N`, the case name is registered. The control is cheap and it is the
  only thing that separates a gate from a claim.

Instance detail follows for the one that cost the most.

## A PCRE lookahead in `HeaderFilterRegex` silently discards every header diagnostic

### Symptom

The `clang-analysis` CI job passes. clang-tidy reports nothing from any header,
ever, on any check — and clang-tidy's own summary line says so if you read it:

```text
Suppressed 2 warnings (2 in non-user code).
Use -header-filter=.* to display errors from all non-system headers.
```

Nothing errors. The configuration is accepted without complaint, so the job
looks like a working gate.

### Root cause

`.clang-tidy` carried:

```yaml
HeaderFilterRegex: '^(?!.*[\\/]tests[\\/]external[\\/]).*$'
```

`(?!...)` is a PCRE negative lookahead. clang-tidy matches header filters with
`llvm::Regex`, which is POSIX ERE and has no lookahead at all. The pattern
matches no path whatsoever, so every diagnostic whose location is a header is
classified as non-user code and dropped. clang-tidy does not diagnose the
unusable pattern; it just matches nothing.

What that costs here is not marginal. In this repository class definitions live
in headers, so a check like `cppcoreguidelines-pro-type-member-init` reports at
the constructor in the `.hpp` and is discarded every time.
`.claude/skills/cpp-coding/references/coding-standard.md` names that check as
the thing covering the indeterminate-member cases its own Python recognizer
cannot see, and names the `clang-analysis` job as the only enforcement for part
of `## Ownership`. Both statements were true about the intent and false about
the effect: the check had never fired on anything.

### Fix

Write the exclusion in POSIX ERE. `llvm::Regex` supports alternation, character
classes and anchors, and a negated-set construction expresses "does not
contain" without lookahead. Whatever form is chosen, prove it with the
regression check below rather than by reading it.

Treat any regex handed to an LLVM tool as ERE unless that tool documents
otherwise. A PCRE-shaped pattern will usually be accepted and then match
nothing, which is the failure mode with no error message.

### Regression check

Falsify the filter, do not inspect it. In a scratch directory, beside a copy of
the repository's `.clang-tidy`:

```cpp
// probe.hpp
#pragma once
struct Probe
{
    int value;
    Probe() {}
};
```

```bash
clang-tidy probe.cpp -- -std=c++23                      # must report the header
clang-tidy --header-filter='.*' probe.cpp -- -std=c++23 # the positive control
```

The second run is the control: it must report
`cppcoreguidelines-pro-type-member-init` at `probe.hpp`. If the control reports
and the first run does not, the repository's filter is matching nothing. With
the lookahead pattern in place that is exactly what happens — confirmed on
clang-tidy 19.1.5 on Windows and on clang 23.1.0 on Linux, so it is a property
of `llvm::Regex` and not of one toolchain.

Keep the control in the loop whenever the filter changes. A filter is the one
piece of lint configuration whose failure looks identical to success.

### The blast radius is not CI-only

clang-tidy is not a Linux job. `CMakePresets.json` carries an `x64-analysis`
configure preset inheriting `x64-debug` with `CPP_ENABLE_CLANG_TIDY=ON`, and
`cmake/static-analysis.cmake` has an MSVC branch adding `--extra-arg=/EHsc`
precisely so it runs there. So the dead filter suppressed header diagnostics on
every developer's Windows analysis run as well as in CI, for as long as it stood.
When the filter starts matching, the diagnostic count rises on both.
