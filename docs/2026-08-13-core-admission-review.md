# Retroactive core admission review

Status: completed review evidence for `O-005`
Date: 2026-08-13
Import audited: `79e6b3d` (2026-07-20)

This review applies `evaluate-core-capability`'s capability kernel to the fifteen
files from the original template import that remain under
`modules/core/source/core/`. It is a one-time retroactive admission pass, not an
open invitation to re-litigate core. The four imported facilities previously
rejected by the same process are already deleted; `project.hpp` also no longer
exists. The fifteen remaining imported files all have production callers.

## Criteria and alternatives

Each ruling asks whether C++23 or an adopted dependency already expresses the
contract, whether the facility removes an observed invalid state or repeated
control flow, whether that contract is enforceable, whether the implementation
is portable and auditable, and whether concise retained evidence can protect
it. `std::expected`, `std::optional`, `std::variant`, ordinary RAII and standard
ownership remain the defaults. A ruling to keep below therefore keeps only the
small contract layered on those facilities; it does not admit a parallel
runtime.

The caller sweep was measured on 2026-08-13 outside `core` and its own tests.
Representative production uses are named instead of treating a dated count as
a permanent contract.

## File-by-file rulings

| Imported file | Demonstrated need and alternative | Ruling | Retained contract and evidence |
|---|---|---|---|
| `error/contracts.cpp` | Release-active invariant failure and safe unreachable handling are used throughout product modules. `assert` disappears in release and direct `std::unreachable` is forbidden. | **keep** | One non-returning diagnostic/abort implementation behind the macros in `contracts.hpp`; no exception or recovery semantics. |
| `error/contracts.hpp` | Engine, vision, domain, image, JSON, Operator and deployment code all require one release-active contract vocabulary. | **keep** | `UF_ASSERT` is debug-only; `UF_CHECK` and `UF_UNREACHABLE` remain release-active. Existing callers exercise the boundary; no speculative validation is added. |
| `error/error.cpp` | Recoverable failures need structured detail/native codes, source location and context with explicit ownership. `std::error_code` alone does not carry that record. | **keep** | Move-only, one-owner payload and deliberate `clone`; `test-error` protects context and clone behaviour. |
| `error/error.hpp` | The public error type is used by the shared `Result` surface across product modules. | **keep** | One-pointer success-path footprint, stable payload views across owner moves, no shared ownership and no implicit copy. |
| `error/result.hpp` | Production code repeatedly propagates the same move-only error and context. C++23 supplies the container but not the project error or propagation statement. | **keep** | `Result<T>` remains an alias of `std::expected<T, Error>`; helpers add no second container. `test-error` protects success extraction, error moves and context. |
| `numeric/checked-arithmetic.hpp` | Image extents, frame geometry, budgets and sequence arithmetic have real overflow/underflow states. C++23 has no general checked integer arithmetic facility. | **keep** | Five integer operations return `std::optional`; no saturation or unchecked fallback. `test-checked-arithmetic` covers boundaries and division traps. |
| `numeric/checked-cast.hpp` | Entry points and product modules repeatedly narrow external sizes and floating measurements. `std::in_range` covers only the integer half. | **keep** | Integer narrowing and finite integral floating conversion return `std::optional`; no rounding. Tests cover narrowing, fractions, infinities and NaN. |
| `safety/annotations.hpp` | Lifetime, no-escape, unsafe-buffer and lock contracts must be visible to Clang while remaining portable to MSVC. C++23 has no equivalent attributes. | **keep** | Analysis attributes on supporting Clang, empty portability spellings elsewhere, and narrow unsafe-buffer brackets. Safety/analysis gates consume them. |
| `safety/checked-access.hpp` | Span/range indexing occurs in parser, image, vision, engine and deployment code; accepting temporary owners would create dangling pointers. | **keep** | Checked access for spans and lvalue contiguous ranges only. `test-checked-access` protects bounds and the temporary-owner exclusion. |
| `time/monotonic-time.hpp` | Capture, engine, controller, task and script paths need one monotonic unit with explicit overflow and saturating elapsed time. | **keep** | Small value types over monotonic ticks; checked addition and saturating subtraction only. `test-monotonic-time` protects both boundaries. |
| `types/enum-reflection.hpp` | Domain, JSON and CLI error vocabularies require exact sparse enum/name conversion. C++23 cannot enumerate declared constants. | **keep** | Explicit registration, compile-time unique names/values, optional lookup; no signature parsing or automatic wire guarantee. `test-capabilities` covers round trips and unknowns. |
| `types/strong-id.hpp` | Domain and controller identifiers must not mix, and generations must not wrap. Plain integers make both invalid states representable. | **keep** | Tagged unsigned IDs plus explicit, non-wrapping `Generation::next`. `test-strong-types` protects separation and exhaustion. |
| `types/strong-value.hpp` | Domain IDs, names and ledger values use tag separation while retaining value ownership. C++23 has no nominal typedef. | **keep** | Minimal explicit value wrapper and matching hash adapter; no implicit conversion or framework behaviour. `test-strong-types` protects separation and hashing. |
| `utility/scope-exit.hpp` | Script FFI, task-host and platform cleanup have real early-return paths. Scope guards are not in the C++23 base library. | **keep** | Move-only, non-throwing cleanup with explicit release; no macro or success/failure variants. `test-capabilities` protects armed, released and moved behaviour. |
| `utility/variant-match.hpp` | Engine, controller, task and trace repeatedly require exhaustive `std::variant` visitation. C++23 retains visitor ceremony. | **keep** | `Overload` plus a forwarding `std::visit` helper only. `test-capabilities` protects explicit handling of every alternative. |

## Result

All fifteen imported files are admitted as the smallest enforceable contracts
already used by production code. None is ruled migrate or delete, so this review
creates no source change request. Deliberate exclusions remain those in the
capability kernel: no custom option/result container, borrow checker imitation,
runtime reflection, executor, allocator, intrusive ownership, or speculative
generic facility.
