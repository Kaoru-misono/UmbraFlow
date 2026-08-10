# Rust-Informed C++23 Capability Kernel

This document records which Rust capabilities belong in the reusable C++23
project core, which ones should use the standard library directly, and which ones
must wait for a concrete product requirement. The goal is better C++ semantics
and ergonomics, not Rust-shaped syntax.

## Selection rule

A facility belongs in `core` only when it meets all of these conditions:

1. C++23 does not already provide an equally clear standard facility.
2. The type removes a real invalid state, lifetime hazard, or repeated control-flow pattern.
3. Its contract is enforceable in ordinary C++ rather than merely suggested by its name.
4. The implementation is small enough to audit and portable across the supported toolchains.
5. It has useful behavior that can be protected by a concise retained test.

## Admission history

Surveyed 2026-08-11 against `55bd564`. It is load-bearing when reading the
Core additions table below: **that table lists what is admitted, not what this
gate evaluated.**

19 of the 27 files under `modules/core/source/core/` arrived in the repository's
first commit, `79e6b3d` (2026-07-20), as a template import. At that commit
`modules/` held nothing but `core`, so no product code existed to demonstrate a
need — and this skill did not exist either, having been written two days later
in `98e0d63`. The import could not have passed a gate that postdates it.

Every facility admitted after product code existed has a production caller
outside `core`: `text/utf8.hpp`, `types/integer.hpp` and
`text/unsafe/unicode-code-unit.hpp` (`91d1e35`, 2026-07-21),
`time/poll-sleep.hpp` (`f146329`, 2026-07-29), `text/json-text.hpp`
(`847e55f`, 2026-08-09). The facilities whose only includer is
`tests/core/test-capabilities.cpp` are all imported ones. So a facility here
with no caller but its own capability test is evidence of an admission that
never happened rather than of an adoption that failed, and the gate works
whenever it is actually run.

Two consequences. A facility's presence in `core` is not a decision, so never
cite the table below as the reason to keep one; find the evaluation that
admitted it, or run one now. And the 2026-07-20 import has never been reviewed
retroactively — one pass over the 19, not an open-ended re-litigation — which is
tracked as W12 in `docs/plans/2026-08-10-next-block.md`.

## Standard library first

Use these facilities directly rather than introducing project copies:

- `std::optional` for absence and `std::expected` for recoverable failure.
  C++23 `std::expected` already has `and_then`, `or_else`, `transform`, and
  `transform_error`; the project adds its error type, context, and propagation
  syntax, not another result container.
- `std::variant` for closed state sets. `Overload` and `matchVariant` only remove
  the current `std::visit` ceremony; they do not replace the variant.
- C++23 ranges for iterator pipelines, including the standard zip, chunk, slide,
  adjacent, and conversion facilities available in the selected toolchain.
- Values, `std::unique_ptr`, `std::shared_ptr`, `std::weak_ptr`, `std::span`, and
  `std::string_view` for normal ownership and view semantics.
- `std::move_only_function`, `std::jthread`, `std::stop_token`, `std::call_once`,
  and thread-safe function-local static initialization where they fit.

The C++23 standard-library direction is supported by WG21's
[`std::expected` monadic operations](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2505r5.html)
and [C++23 ranges plan](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2214r2.html).

## Core additions

These facilities fill a material C++23 gap without creating a parallel runtime:

| Facility | Capability | Deliberate limit |
| --- | --- | --- |
| `Result<T>`, `Status`, `fail`, and `UF_TRY*` | Structured errors, context, and value propagation | Aliases `std::expected`; `fail` only creates `std::unexpected<Error>`; no wrapper container |
| `Overload` and `matchVariant` | Concise, compile-time-complete handling of `std::variant` alternatives | No attempt to parse or emulate language patterns |
| `ControlFlow` | Named early exit with optional break/continue values | A closed sum type, not coroutine control flow |
| `NonZero<T>` | Makes zero invalid after construction | No Rust-style niche-layout or ABI guarantee |
| `ScopeExit` | Deterministic rollback and C-boundary cleanup | Cleanup must be non-throwing; no macro syntax |
| `Flags<E>` | Type-safe flag sets without global enum operators | No complement operator or unchecked raw-bit constructor |
| `EnumTraits<E>` and enum conversion helpers | Exact enum-to-name and name-to-enum mapping validated at compile time | C++23 registration is explicit; names are not an automatic wire-format contract |
| `Synchronized<T>` | Couples mutable data to its mutex and scoped operation | Cannot prove that a callback does not hide an alias inside another object |
| Safety annotations | Makes lifetime, no-escape, unsafe-buffer, and lock contracts visible to supported Clang analysis | Empty portability macros on compilers without the corresponding analysis |
| `tryAt` and `checkedAt` | Checked access for spans and lvalue contiguous ranges without accepting temporary owners | Pointer result is still a non-owning call-scoped observation |
| Strong values, checked numeric operations, monotonic time | Domain separation and explicit numeric/time failure | Remain small value facilities rather than a framework |

Rust's standard library motivates the explicit early-exit, non-zero, and
lock-coupled shapes: [`ControlFlow`](https://doc.rust-lang.org/std/ops/enum.ControlFlow.html),
[`NonZero`](https://doc.rust-lang.org/std/num/struct.NonZero.html), and
[`MutexGuard`](https://doc.rust-lang.org/std/sync/struct.MutexGuard.html).
The C++ pattern-matching proposal also shows that C++23 still requires verbose
visitor machinery for variants; `matchVariant` is a narrow bridge until the
language gains a standard construct. See
[P2688R4](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2688r4.html).

`ScopeExit` is included because scope guards remain in the Library Fundamentals
v3 TS rather than the base C++23 library. See the
[TS scope-guard specification](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/n4948.html)
and the committee discussion explaining why the material was too late for
[C++23](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2631r0.html).

## Enum string reflection

C++23 cannot enumerate an enum's declared constants through a standard language
facility. Register the intended closed set once at global namespace scope, using
a qualified enum type when necessary:

```cpp
enum class BuildState
{
    Idle = 1,
    Running = 4,
    Failed = 9
};

UF_REFLECT_ENUM(
    BuildState,
    BuildState::Idle,
    BuildState::Running,
    BuildState::Failed
);
```

`enumName(BuildState::Running)` returns `"Running"`, while
`enumFromName<BuildState>("Running")` returns the enum value. Unknown values and
names return `std::nullopt`. Registration supports sparse and negative values and
does not scan an arbitrary integer range. Empty names, duplicate names, duplicate
values, and therefore ambiguous enum aliases are rejected at compile time.

The macro only stringizes the explicitly listed enumerators. It does not parse
compiler function signatures, and the mapping remains available through the
ordinary `EnumTraits<E>` specialization point when stable external labels differ
from C++ identifiers. Renaming a C++ enumerator also renames its macro-generated
label, so use explicit stable labels plus format versioning for persistent wire or
disk formats.

Future standard reflection provides `std::meta::enumerators_of` and includes an
enum-to-string example. The public conversion API can adopt that backend when the
project moves beyond C++23 without changing callers. See
[P2996R13](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p2996r13.html).

## Product-level candidates

These are valuable, but they should be added as separate modules only when a
project demonstrates the need:

- A hardened generational `Handle` and `SlotMap` for entities, tasks, resources,
  or registrations. It must reject stale handles in release builds, check
  capacity, keep raw representation private, and report generation exhaustion
  rather than wrap.
- `Signal` plus an RAII connection for event-heavy applications. Its threading,
  reentrancy, callback lifetime, and emission-order policy must be explicit.
- A structured `TaskGroup` and bounded `Channel<T>` when the product actually
  needs asynchronous orchestration. They must propagate cancellation, join all
  child work, and define error handling; detached work is not acceptable.
- An `EnumMap`, generic native-resource owner, or serialization primitives when
  repeated use establishes a stable API.

The task layer is intentionally deferred. `std::execution` and its structured
concurrency model are still an active standardization area, including async-scope
work that preserves joining semantics. Building a general executor into this
project now would create a large compatibility and correctness burden. See
[P2300R9](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r9.html)
and [P3109R0](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p3109r0.html).

## Rejected baseline abstractions

Do not add these to the shared core without new evidence:

- `Borrow`, `Pin`, `RefCell`, or a non-null wrapper presented as lifetime safety.
  C++ aliases can escape and raw access remains available, so these names cannot
  reproduce Rust's compiler-enforced contract.
- Custom `Arc`, `Weak`, `Option`, `Result`, iterator/range, or coroutine-runtime
  replacements where the standard facility already fits.
- A non-owning `FunctionRef` that binds temporaries. It is easy to store a
  dangling callable view, and the shared core has no borrow checker to
  prevent it.
- Intrusive object ownership, small-vector implementations, aggregate/runtime
  reflection, VFS, job systems, or allocators without a measured product need.

Rust's `Pin` documentation explicitly describes an address-sensitive library
contract built around restrictions on access and movement. Those restrictions
cannot be recreated generically while ordinary C++ aliases remain unrestricted;
see [`std::pin`](https://doc.rust-lang.org/std/pin/).

## Review trigger

Revisit a deferred facility only when at least two real call sites repeat the
same contract, a benchmark demonstrates a container need, or a product subsystem
requires a stable cross-module abstraction. Promote the smallest enforceable
contract, keep unsafe implementation details behind a boundary, and retain only
tests that protect the public behavior.
