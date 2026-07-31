# Safe C++ Core and Repository Gates

> **词汇重定向(2026-07-31)。** 本文是有日期的记录,不改写。下文的
> `recognizer` / `RecognizerId` / `uf.recognizers` / `recognizerId` 一律读作
> **element** / `ElementId` / `uf.elements` / `elementId`;`RecognizerDefinition`
> 与 `RecognizerVariant` 读作 `CompiledElement` 与 `CompiledAppearance`;
> `Variant` / `variant` 读作 `Appearance` / `appearance`。`RecognitionCatalog` 与
> `RecognitionRuntime` 名字不变——它们指的是「识别」这个动作。schema id 随改名一起动了:
> `umbraflow-authoring/v4`、`umbraflow-annotations/v3`、`umbraflow-trace/v2`。
> 权威词汇见 `CONTEXT.md` 的「Annotation model」一节。

> 状态:已完成并归档(2026-07-24)——内容已吸收进 cpp-coding 技能资料与 `CLAUDE.md` 的安全和能力评估路由。

## Goal

Give C++ high-value capabilities and ergonomic structures inspired by Rust
without trying to reproduce the Rust language. Keep the reusable core small,
standard-library-friendly, and explicit at genuinely unsafe boundaries.

## Completed scope

- Added strict warnings, hardening, clang-tidy, and sanitizer CMake layers.
- Applied the safety profile to generated modules, entry targets, and tests.
- Replaced the include-only core source with matching contract and error units.
- Added structured results, release-safe contracts, strong domain values,
  non-wrapping generations, checked arithmetic, checked casts, and monotonic time.
- Added value-carrying result propagation, concise variant matching, explicit
  traversal control flow, non-zero values, non-throwing scope cleanup,
  type-safe flags, validated enum string conversion, and lock-coupled mutable state.
- Added module graph and unsafe-boundary validation scripts.
- Added retained tests for structured error propagation, arithmetic boundaries,
  strong identifiers, generation exhaustion, and monotonic-time overflow.
- Documented ownership, borrowing, concurrency, serialization, and unsafe rules.

## Deliberate exclusions

Intrusive references, custom small containers, non-owning callable wrappers,
serialization, VFS, aggregate/runtime reflection, job systems, task runtimes, channels, and
profiling remain outside the shared core. Generational slot maps and
signals are documented product-level candidates, not universal primitives. They
add policy or unsafe implementation surface and must be introduced by a product
only after a demonstrated requirement.

The research matrix and promotion criteria are recorded in
the
[`evaluate-core-capability`](../../../.claude/skills/evaluate-core-capability/SKILL.md)
skill.
