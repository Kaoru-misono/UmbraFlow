# C++23 Capability and Safety Profile

This repository borrows useful capabilities and structures from Rust where they
make C++ clearer and easier to use. It does not attempt to turn C++ into Rust or
reject normal idiomatic C++ merely because Rust would express it differently.

The capability kernel currently adds structured results and value propagation,
variant matching, explicit traversal control flow, non-zero values, strong domain
values, type-safe flags, validated enum names, non-wrapping generations, checked
arithmetic, deterministic scope cleanup, lock-coupled state, and process-local
monotonic time.
New facilities must compose with the standard library and remove real boilerplate
or misuse rather than imitate another language. The full selection matrix lives
in the `evaluate-core-capability` skill's
[`capability-kernel.md`](../../evaluate-core-capability/references/capability-kernel.md).

## Safety layers

The profile uses four complementary layers:

1. Types prevent domain mixing and invalid construction.
2. Compiler and static-analysis gates reject risky source patterns.
3. Runtime contracts and checked operations prevent silent corruption.
4. Sanitizers and boundary tests detect defects that C++ cannot prove away.

Sanitizers are test-time evidence, not a language-level safety guarantee. The
safety rules target undefined behavior and unclear lifetime boundaries without
prohibiting useful C++ patterns that have a clear local contract.

## Ownership and borrowing

Use ownership in this order:

1. Values.
2. `std::unique_ptr<T>` for exclusive dynamic ownership.
3. `std::shared_ptr<T const>` for shared immutable data, or `std::shared_ptr<T>`
   when synchronized mutable ownership is an explicit part of the API.
4. `T&` or `T const&` for required synchronous borrows.
5. `T*` only for optional, non-owning observation with a documented lifetime.

Raw pointers never own resources. Application code does not use raw `new` or
`delete`. Native handles, callbacks, registrations, and mapped memory use RAII.
Ownership is expressed by members and API signatures rather than a universal
base class or a parallel pointer hierarchy.

Views such as `std::span` and `std::string_view` are normally call-scoped. Do not
store or return a view unless the API makes the backing owner's lifetime explicit.
A non-null pointer wrapper does not prove a lifetime and must not be treated as a
borrow-checker substitute.

References are required synchronous borrows. Raw pointers are optional,
non-owning observations of one object and never pointer-plus-size buffer APIs.
Pointer, reference, iterator, and view members are forbidden by default; an
exception must identify the backing owner, prove that it outlives the borrow, and
expose that dependency in the API. Intentional returned borrows use the project
lifetime-bound annotation.

A function that retains shared ownership takes `std::shared_ptr<T const>` by
value. A function that only observes the object takes `T const&`. Mutable shared
ownership and `std::enable_shared_from_this` require a documented synchronization
and lifetime reason.

Derive observers after moving ownership to its destination. Stored and
asynchronous work captures owned state or locks a `std::weak_ptr` when it runs;
it does not retain reference captures or a bare `this`.

## Valid states only

Types with invariants use private state and establish the invariant during
construction. If validation can fail, expose a named factory returning
`Result<T>`. Avoid two-phase initialization and public aggregates whose fields
can represent contradictory states.

Use a public constructor when construction cannot fail. Use a named static
factory when establishing the type's own invariant can fail. Put creation on an
owning capability when construction requires that capability. These are API
shapes, not a requirement for a common object base or product-specific manager.

Use `StrongValue` or `StrongId` for identifiers, units, coordinate spaces, and
other domains that share a primitive representation but must not mix. Use
`std::variant` for state machines when an enum plus optional fields would admit
invalid combinations.

## Errors and contracts

- Recoverable runtime failure with a value: `Result<T>`.
- Recoverable runtime failure without a value: `Status`.
- Debug-only invariant evidence: `UF_ASSERT`.
- Mandatory release invariant: `UF_CHECK`.
- Impossible control flow: `UF_UNREACHABLE`.

`Result<T>` is an alias for `std::expected<T, Error>`, and `Status` is
`Result<void>`. Return either failure with the unified `fail(...)` helper. Use
`UF_TRY*` for linear propagation or normal `std::expected` operations
for monadic composition. The value-extracting macros are standalone statements
and require a braced block. Result-returning functions are `[[nodiscard]]`.

Do not use a result as an ordinary branch signal, optional value, lookup miss,
loop exit, or per-frame hot-path state. Use `std::optional`, `bool`, a domain
enum, `std::variant`, or `ControlFlow` instead.

External input is validated before it reaches unchecked standard-library APIs.
An assertion must never be the only guard before a memory access. Low-level code
returns structured errors, intermediate layers add context, and a subsystem or
application boundary logs the error once.

Direct `std::unreachable` is forbidden. The project contract implementation
reports and terminates first, so an accidentally reached path cannot become
undefined behavior.

## Arithmetic and conversions

Signed overflow is undefined behavior in C++. Size, offset, generation, and time
calculations use the checked helpers in `core/numeric`. Narrowing conversions use
`checkedCast`; floating-point input is checked for finiteness before range or
ordering decisions. A generation counter must report exhaustion instead of
wrapping and making a stale handle valid again.

Dynamic indices from input or external APIs require a checked access path.
Use `tryAt` when an invalid index is recoverable and `checkedAt` when the index is
a mandatory internal precondition. Unchecked `operator[]` is reserved for loops
whose bounds are locally proven.

## Concurrency

C++ cannot infer Rust-style thread-safety traits. Prefer thread confinement and
message passing:

- Use `std::jthread` and `std::stop_token`; never detach a thread.
- Synchronous non-escaping callbacks may capture references or `this` when the
  lifetime is local and obvious.
- Stored or asynchronous work captures by value, move, or an explicit
  lifetime-owning handle.
- Send owned values or immutable snapshots between threads.
- Prefer `std::shared_ptr<T const>` over shared mutable state.
- Bind unavoidable mutable shared state to its lock and never leak references
  outside the protected scope.
- Prefer `Synchronized<T>::withLock` when one value is always governed by one
  mutex; return owned results rather than pointers or references to its storage.
- Do not call unknown callbacks while holding a lock.

Run ThreadSanitizer on a supported Clang or GCC platform. It supplements this
architecture but does not replace it.

## Unsafe and platform code

The following operations are forbidden in normal source directories:

- `reinterpret_cast` and `const_cast`.
- Raw allocation and C allocation APIs.
- Pointer arithmetic and manually managed object lifetime.
- Detached threads.
- Direct `std::unreachable`.

Necessary operations live under `unsafe/`, `platform/`, or `ffi/`. Each operation
must have a nearby comment beginning with `// SAFETY:` that states the lifetime,
bounds, alignment, aliasing, and thread assumptions that make it valid. The
boundary must expose a safe value or RAII type rather than propagating raw state.

## Serialization

Do not serialize a C++ object by copying its in-memory representation. Wire and
disk formats define endianness, version, limits, and allocation quotas explicitly.
Decode into an untrusted transport representation, then construct the domain type
through its validating factory. Process-local monotonic instants are never
serialized.

## Automated gates

Every project target receives the generic safety profile from
`cmake/safety-profile.cmake`:

- Strict warnings and warnings-as-errors.
- Compiler and linker hardening.
- Clang unsafe-buffer and thread-safety diagnostics when supported.
- Clang high-confidence lifetime diagnostics, required by the pinned analysis
  preset and CI job.
- clang-tidy analysis in the required CI analysis job.
- Address, undefined-behavior, and thread sanitizer configurations.

The repository checks are:

```bash
python scripts/fix_format.py --check
python scripts/check_cpp_format.py
python scripts/check_modules.py
python scripts/check_safety.py
```

The hosted CI workflow requires the repository gates, GCC, Apple Clang, MSVC,
the pinned Clang lifetime/tidy analysis lane, and separate ASan, UBSan, and TSan
jobs. Local presets may keep expensive analysis opt-in, but a pull request cannot
silently omit it. The Clang analysis remains bug finding rather than a proof of
Rust-style lifetime safety.

An exception to a safety rule must be narrow, documented, and placed at the
correct boundary. Disabling a check for an entire target is not an acceptable
local workaround.

## Test retention

Tests exist to preserve valuable behavior, not to maximize their count. Keep the
smallest set that covers public contracts, important boundary values, invariants,
bug regressions, and compatibility-sensitive behavior. Prefer table-driven cases
when one behavior has several representative inputs.

Diagnostic, characterization, and implementation-scaffolding tests may be added
while developing a feature. Delete them when the feature is complete unless they
protect a behavior that must remain stable. Remove tautologies, trivial accessor
tests, duplicated cases, and tests tied only to private implementation details.
