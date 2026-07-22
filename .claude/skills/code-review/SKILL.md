---
name: code-review
description: Multi-dimensional review of a local diff — fan out one agent per dimension (rules, bugs, git history, in-code constraints, test coverage, over-engineering), score each finding 0-100, drop findings scored below 80, and synthesize one report. Use when the user says "review my changes", "code review", "看看这段代码", "审查代码", or "check this diff" — committed or working tree, any scope. C++ changes also load the cpp-coding rules directly.
---

# Code review

Two principles: **different failure modes need different lenses** (fan out one agent per
dimension — a single agent skims), and **most flagged "issues" are false positives** (score each
0-100, drop <80, so the report carries only what a Staff engineer would act on). Rule source is
this project's `CLAUDE.md` + `cpp-coding`; review is local (no GitHub PR — work lives in
`docs/plans/`), build/test is `post-change-validation`'s job, not this skill's.

## Scope

- **No argument** → `git diff HEAD` (whole working tree).
- **Named paths** → those files only.
- **`--base <ref>`** → `git diff <base>...HEAD`.
- **`--staged`** → `git diff --cached`.

Capture the diff once; pass the same scope to every agent. **Exclude unrelated changes** — if
the working tree has another session's work, restrict to the target files explicitly. Never
`git add`/stash/checkout during a review. Non-C++ files (Slang/CMake/JSON/Python/Markdown) are
in scope for every dimension **except** the cpp-coding rule dimension.

## Procedure

1. **Eligibility / scope.** Confirm the diff is non-empty and worth reviewing.
   Trivial diffs (whitespace, generated metadata, a pure
   rename) → say so and stop.
2. **Gather rule sources.** Read `CLAUDE.md` (root) and any module `CLAUDE.md` /
   `README.md` under touched directories. For C++ changes, the rule sheet is the
   `cpp-coding` skill (+ its `references/*.md`).
3. **Fan out one agent per dimension** (single message). Each returns findings as
   `{title, file:line, dimension, why}`.
   - **A — Rule & CLAUDE.md compliance.** Does the change follow `CLAUDE.md`
     (ownership, pitfalls, workflow) and, for C++, `cpp-coding`? Cite the
     rule. For C++, load `cpp-coding` and apply the mandatory ownership and
     lifetime lens below as part of this dimension.
   - **B — Bug hunt (shallow).** Diff only. Large, real bugs: wrong key/type,
     off-by-one, inverted condition, wrong include/exclude, null-vs-missing. Skip
     nitpicks and anything a compiler/linter catches.
   - **C — Git history.** `git log`/`git blame` the touched lines. Flag
     regressions against past intent — a deliberately-added guard now removed, a
     reverted experiment reintroduced.
   - **D — In-code constraints.** Comments + `docs/pitfalls/` near the change.
     Flag violations of guidance written *in the code* ("must be trivially
     destructible", "no uploads mid-pass").
   - **E — Test coverage.** Does an existing test exercise the changed behavior?
     Missing coverage is a finding, not a style note (the project rule: a passing
     build is not enough).
   - **F — Over-engineering.** Complexity the diff *adds*. One line per finding:
     `<file>:L<line>: <tag> <what to cut>. <replacement>.`
     - `delete` — dead code, speculative flexibility, unused feature.
     - `stdlib` — hand-rolled what the stdlib ships (name it).
     - `reuse` — re-written what `core`/a module already has (name it).
     - `yagni` — one-implementation abstraction, unset config, single-caller layer.
     - `shrink` — same logic, fewer lines (show it).
     End `net: -<N> lines possible.` Scope: complexity only — correctness/perf
     are other dimensions. A `TODO(cpp-debt):` marker is an intentional
     shortcut, never flag it.
4. **Score every finding 0-100** (one quick Haiku/Sonnet agent per finding, or
   batch them). Rubric, given verbatim to the scorer:
   - **0** — false positive or pre-existing; doesn't survive light scrutiny.
   - **25** — maybe real, maybe not; unverifiable, or stylistic & not in CLAUDE.md.
   - **50** — real but a nitpick / rare in practice / unimportant vs the rest.
   - **75** — verified very likely real, hits in practice, important; or directly
     called out in CLAUDE.md.
   - **100** — definitely real, happens frequently, evidence directly confirms.
5. **Filter < 80.** Drop anything below 80. If nothing survives, report clean.
6. **Synthesize** one structured report (see format). Do **not** post anywhere —
   this is a local review. Cite `file:line` (clickable), not GitHub URLs.

## Mandatory ownership and lifetime lens

Every C++ review must explicitly inspect the following, even when no finding is
ultimately reported:

1. Each changed API makes ownership transfer, retention, and borrowing visible
   in its parameter, return, and member types.
2. Raw pointers are optional non-owning observations of one object; no raw
   pointer owns memory or substitutes for a pointer-plus-size buffer.
3. Pointer, reference, iterator, and view members have a backing owner that is
   explicit and provably longer-lived, or are rejected.
4. Returned references and views cannot originate from temporaries, value
   parameters, moved owners, reset owners, or undocumented invalidation-prone
   storage.
5. Exclusive ownership uses values or `std::unique_ptr`; retained shared
   ownership is justified and prefers `std::shared_ptr<T const>`.
6. Mutable shared state has synchronization in the owning API and does not leak
   an alias outside the protected scope.
7. Stored and asynchronous callbacks do not retain reference captures or a bare
   `this`; weak observations are locked at execution time.
8. Native resources and registrations have non-throwing RAII cleanup.
9. Construction establishes invariants once; factories follow the constructor,
   type-factory, owning-capability order and do not introduce two-phase init.
10. Unsafe operations remain in a narrow boundary with a concrete `// SAFETY:`
    proof covering lifetime, bounds, aliasing, alignment, and threading.

These checks are semantic review responsibilities. Do not drop a real ownership
or lifetime violation merely because a regex gate or current compiler version
does not diagnose it.

## False-positive checklist (hand to every agent)

Drop these without scoring:

- Pre-existing issues on lines the change didn't touch.
- "Looks like a bug" that isn't (verify against surrounding code first).
- Pedantic nitpicks a senior engineer wouldn't raise.
- Anything a compiler / linter / typechecker / `clang-format` / the doc-gen gate
  catches (missing imports, type errors, format, doc-index drift). CI/local
  validation runs these separately — don't duplicate.
- General "code quality" (test coverage is in scope via dimension E, but vague
  "could be cleaner" is not) unless a rule explicitly requires it.
- Issues the code explicitly silences (a `// NOLINT`, an intentional `void` cast).
- Intentional behavior changes tied to the broader change.
- **Project-specific:** known pre-existing test failures unrelated to the diff
  (check `docs/pitfalls/` and recent session context — e.g. an unrelated
  screenshot/image-codec failure is not this change's bug).

## Synthesis format

Lead with a one-line verdict (CLEAN / N issues). Then per surviving finding:

```
[BUG|SHOULD|NIT] <one-line title>  (confidence: NN)
  <file>:<line> — <what's wrong, why it's real, concrete fix>
```

Group by severity. End with a coverage note (dimension E): which tests cover the
change, or "no test exercises this — add one" with a suggested location. Then a
single over-engineering line (dimension F): `net: -<N> lines possible.` (or
`Lean already.` if nothing to cut).

## Notes

- Prefer Sonnet subagents for the six dimensions (mechanical, high-volume);
  reserve the main loop for synthesis and the hard judgment calls.
- Do not build or run tests as part of the review itself — that is
  `post-change-validation`'s job, run separately. The review only *identifies*
  that coverage is missing.
- For a C++ change, dimension A is the `cpp-coding` conformance pass. Do not
  dispatch a second duplicate rule review.
- Keep the final report brief. No emojis. Every claim cites `file:line`.
