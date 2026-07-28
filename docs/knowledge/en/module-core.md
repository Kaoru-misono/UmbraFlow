# modules/core: The Platform-Independent Capability Kernel

This document explains the responsibilities, public capabilities, and extension boundaries of
`modules/core`. It is aimed at developers who are entering the repository for the first time and
need to locate implementations along the real interfaces and safely extend subsystems. For the
overall architectural constraints, see "Core capability kernel" in `docs/ARCHITECTURE.md`; the
discussion below takes the actual headers and retained tests under
`modules/core/source/core/` as authoritative.

## Module Scope

`core` is the platform-independent leaf node at the very bottom of the module graph.
`modules/core/manifest.txt` contains only `[module]` and `[build]`, with no `[dependencies]`;
therefore other modules may depend on it, but it may not depend on any project module.
`scripts/check_modules.py` rejects any link dependency declared by `core` and verifies that the
entire module graph is acyclic. This is not an accidental state of the build files but the
precondition that lets the kernel be reused in common by `domain`, `vision`, `image`,
`annotation`, `engine`, `script`, and the Windows-only `controller`.

What it owns are portable, auditable "small mechanisms" that eliminate a class of invalid state:

- `error/`: recoverable failure, propagation syntax, and contract termination that still takes
  effect in release.
- `control/`: explicit continue/early-exit results for traversal and visitors.
- `concurrency/`: synchronized access that binds a mutex to the value it protects.
- `numeric/`: explicit failure for integer arithmetic, integer conversion, and float-to-integer
  conversion.
- `safety/`: static-analysis annotations and checked indexing into contiguous storage.
- `text/`: UTF-8 validation and scalar decoding/encoding, plus isolated code-unit bit conversion.
- `types/`: fixed-width integers, strongly typed values, identifiers, generation, non-zero values,
  flags, and explicit enum reflection.
- `time/`: an in-process monotonic instant and its overflow-safe arithmetic.
- `utility/`: `std::variant` visitor composition and deterministic scope cleanup.

There is no aggregate `core.hpp`. Callers must include the precise facility they need; this keeps
the dependency surface, compilation cost, and conceptual coupling all visible. Application identity
stays at the entry layer rather than entering this mechanism-only leaf. As decided on 2026-07-28,
the repository-root `manifest.txt` supplies the application name and version to CMake, which
generates an entry-only `application-info.hpp`. Only facilities that require a non-template
implementation have a matching `.cpp`: `error/contracts.cpp`, `error/error.cpp`, and
`text/utf8.cpp`.

`core` deliberately owns no product semantics. It knows nothing of frame, detection, annotation,
engine session, Windows handle, Luau, GUI, or any concrete game, and it does not decide whether a
failure should retry, abort, or be shown to the user. `AutomationErrorKind` and `FailureResponse`
live in `modules/domain/source/domain/error.hpp`, strict-background capture and input live in
`modules/controller`; the lower layer only provides stable representations and checks, while
policy is set by the business owner.

Nor does it duplicate the standard library. `Result<T>` is an alias for
`std::expected<T, Error>`, `ControlFlow` is built on `std::variant`, absence still uses
`std::optional`, and ownership still uses standard smart pointers and value types. The archived
authoritative plan `docs/archive/plans/2026-07-20-safe-cpp-core.md` explicitly excludes intrusive
references, homegrown containers, serialization, VFS, job system, task runtime, channel, and
profiling; these must not enter `core` merely because they "look generic."

## Foundational Capabilities

### error: Constructing, Classifying, and Forwarding Failure

`modules/core/source/core/error/error.hpp` defines the move-only `Error`. The object itself has
only a single `std::unique_ptr<Payload>` data member; the design goal is to keep `Error` one
pointer wide and move diagnostic data onto the heap. The reason lies on the success path: the
`std::expected` of `Result<T>` must pay the space cost of the either-or storage of `T` and
`Error`. A small `Error` avoids having every successful return value inline-carry multiple
`std::string`, `std::vector`, and `std::source_location`; allocation happens only when a failure
is genuinely constructed. The source does not declare an ABI guarantee with
`static_assert(sizeof(Error) == sizeof(void*))`, so "one pointer" should be understood as the
current layout intent, not a cross-implementation serialization contract.

`Error::Payload` holds five kinds of information: `m_detailCode` is the machine-branchable
`std::error_code`; `m_nativeCode` is an optional OS/library error that retains its own category;
`m_message` is a human-readable explanation; `m_location` captures the construction point by
default via `std::source_location::current()`; and `m_context` is the sequence of context strings
appended in order as the error rises through the semantic layers.

Classification does not rely on parsing `message`. The `std::error_code` returned by
`detailCode()` carries both an integer value and a category; the category is the identity of the
error vocabulary. `core`'s `fail(std::error_code, ...)` accepts any vocabulary rather than adding
yet another coarser generic code. For example, `modules/domain/source/domain/error.cpp` defines a
private `AutomationErrorCategory` that encodes `AutomationErrorKind` into the `uf.automation`
category, and then domain's `fail(AutomationErrorKind, ...)` delegates to `uf::fail`. Callers can
identify domain errors precisely, while `nativeCode()` can still independently retain the platform
reason from `std::system_category()`. Classification, native reason, and explanation each have
their own job, avoiding the compression of "what failed" and "which API returned what" into a
brittle string.

`Error` forbids copying and permits noexcept move. During propagation there is only one owner, so
there is no implicit aliasing in which a lower frame and an upper frame share the same mutable
context; when a copy is genuinely required it must call `clone()` explicitly, which deep-copies
the entire `Payload`. A moved-from object may only be destroyed or reassigned: both `payload()`
and `addContext()` verify `m_payload != nullptr` with a release-active `UF_CHECK`, so misuse
terminates rather than dereferencing a null pointer.

The heap payload also yields a stable address: moving an owning `Error` does not move the
`Payload`, so the `std::string_view` from `message()` and the `std::span` from `context()` remain
valid across an `Error` move. They are still borrows: they all become invalid after the payload is
destroyed, and `context()` is also invalidated by the vector reallocation that a subsequent
`addContext()` may trigger. The `UF_LIFETIME_BOUND` on the declaration and the adjacent `SAFETY`
comment expose this lifetime contract to analyzers and readers.

`toString(Error const&)` is the final diagnostic renderer: it outputs the detail category name and
category message, the free text, the construction location, optionally the native
category/value/message, and then each layer of context in insertion order. It does not log;
propagation and the observability boundary are decided by the upper layer, so the same failure is
not reprinted at every layer.

`modules/core/source/core/error/result.hpp` provides:

- `Result<Value>`: `std::expected<Value, Error>`.
- `Status`: `Result<void>`.
- `fail(...)`: constructs `std::unexpected<Error>`.
- `ok()`: constructs a successful `Status`.
- `withContext(result, text)`: appends context in place only on the failure branch, then returns
  the result by value.

`UF_TRY(expression)` first holds the result by value as `auto ufResult = (expression)`. It does
not bind `auto&&`, so it does not let the macro go on to read a dangling reference inside a
temporary `Result`. On failure, the expression `std::move(ufResult).error()` keeps an xvalue,
`std::unexpected` takes ownership of the move-only `Error` and returns immediately from the
current function; on success no extra work is done. If an lvalue `Result` is passed in, the caller
must explicitly `std::move`, which makes the consumption of ownership visible.

`UF_TRY_CONTEXT` differs only in that it first moves the error into a local `ufError`, appends the
context of the current layer, and then move-returns. `UF_TRY_VALUE(name, expression)` executes
`auto name = *std::move(result)` on success, moving the value into the caller's current braced
block; `UF_TRY_VALUE_CONTEXT` additionally provides failure context. The two value macros are
declaration-style macros and must be used as a standalone statement within a braced scope; their
internal result name is concatenated with `__LINE__` to avoid common local-name collisions. All
four macros only forward the original `Error`; they do not reclassify, clone, log, or catch
exceptions.

Unrecoverable programmer defects take another channel.
`modules/core/source/core/error/contracts.hpp` defines `UF_ASSERT`/`UF_ASSERT_MSG`,
`UF_CHECK`/`UF_CHECK_MSG`, and `UF_UNREACHABLE`/`UF_UNREACHABLE_MSG`. `UF_ASSERT` is not evaluated
under `NDEBUG`, retaining only a `sizeof`-level syntax check; `UF_CHECK` and `UF_UNREACHABLE`
still call `detail::contractViolation` in release. The implementation attempts to write the kind,
expression, message, and `source_location` to `std::cerr`, and ultimately `std::abort()`s even if
the diagnostic itself throws. A contract failure is therefore not a recoverable `Error`; the
caller must not continue executing on state whose invariant has already been broken.

### control and utility: Explicit Control Flow and Scoped Behavior

`Continue<Value>` and `Break<Value>` in `modules/core/source/core/control/control-flow.hpp` own
their own `value` and use `static_assert` to forbid a reference payload; the default payload is
`std::monostate`. `ControlFlow<BreakValue, ContinueValue>` is the `std::variant` of the two, and
`isBreak()` and `isContinue()` determine the state by the alternative type. It expresses the local
early exit of a visitor/traversal, not a coroutine, task cancellation, or an exception substitute.

`Overload` in `modules/core/source/core/utility/variant-match.hpp` composes the `operator()` of
multiple callables, and `matchVariant()` then hands off to `std::visit`. All variant alternatives
must be callable at compile time; the facility only eliminates visitor boilerplate and does not
introduce another sum type.

`ScopeExitFunction` in `modules/core/source/core/utility/scope-exit.hpp` requires the callable to
be noexcept-movable and noexcept-invocable. `ScopeExit` is move-only, and move construction uses
`std::exchange` to disarm the source object, ensuring that cleanup is the responsibility of
exactly one object; on destruction it is invoked if still armed, and `release()` can cancel it
explicitly. `scopeExit()` only accepts an rvalue callable, avoiding treating the external callable
itself as an implicitly stored borrow.

### concurrency: Lock and State Are Inseparable

`Synchronized<Value, Mutex = std::mutex>` in
`modules/core/source/core/concurrency/synchronized.hpp` owns both `m_value` and a mutable
`m_mutex`, and is itself neither copyable nor movable. Default construction value-initializes
`Value`; it also supports construction by value and `std::in_place` parenthesized construction.

Reading and writing can only go through `withLock(function)`. The non-const overload hands a
`Value&` to the callback, the const overload hands a `Value const&`, and both are invoked within
the lifetime of a `std::lock_guard`. The return type may be a value or `void`, but
`k_lockResultDoesNotExposeStorage` rejects at compile time directly returning a pointer or
reference, preventing the most obvious out-of-lock alias. It cannot prove that the callback did
not stash an address into another object, so it narrows rather than exaggerates the safety that
C++ can enforce; the lifetime, cancellation, and join of a callback stored across threads remain
the responsibility of the upper layer that owns the concurrent work.

### numeric: Prove the Operation Valid First, Then Execute

`CheckedInteger` in `modules/core/source/core/numeric/checked-arithmetic.hpp` accepts an integral
type but excludes `bool`, plain `char`, `wchar_t`, and the various Unicode character types,
avoiding treating a logical value or code unit as a quantity in arithmetic.

`checkedAdd`, `checkedSubtract`, `checkedMultiply`, `checkedDivide`, and `checkedRemainder` return
`std::optional<Value>`. Each function checks the bounds before executing an expression that could
produce undefined behavior or unsigned wrap. Signed multiply first has `detail::unsignedMagnitude`
compute the absolute magnitude in the unsigned domain, using the magnitude of the positive upper
bound or the negative lower bound respectively, so it can accept `min * 1` while rejecting
`min * -1`. Both divide and remainder reject division by zero and the signed `min / -1` special
case.

`modules/core/source/core/numeric/checked-cast.hpp` separates two semantics.
`checkedCast<To>(integer)` uses `std::in_range` to reject a sign change or narrowing;
`checkedIntegralCast<To>(float)` first rejects NaN/Infinity, then uses `long double`,
`numeric_limits<To>::digits`, and `std::ldexp` to construct the exact half-open range, and finally
rejects any fractional value. All failures are `std::nullopt`, and the caller that owns the
business semantics decides whether it is an input error, a resource error, or a contract defect.

### safety and text: Borrows Are Visible, Byte Boundaries Are Isolated

`modules/core/source/core/safety/annotations.hpp` wraps Clang's lifetime, no-escape,
unsafe-buffer, and thread-safety attributes into `UF_*` macros; on non-Clang compilers the macros
are empty. These are analysis enhancements that do not change runtime semantics and cannot prove
lifetime on their own.

`tryAt(span, index)` in `modules/core/source/core/safety/checked-access.hpp` returns `nullptr` on
an out-of-bounds access, while `checkedAt(span, index)` triggers a release-active `UF_CHECK` on an
out-of-bounds access. The range overload only accepts a contiguous, sized lvalue range from which
a `std::span` can be constructed; `CheckedAccessRange` explicitly rejects a temporary owner,
avoiding a returned pointer or reference after the owner has been destroyed. The return value is
still a call-scoped borrow that the caller must not retain after the container is invalidated.

`isValidUtf8()` in `modules/core/source/core/text/utf8.hpp` accepts the empty string and valid 1–4
byte scalar encodings, and rejects isolated continuations, overlong sequences, truncated
sequences, surrogates, and values greater than `0x10FFFF`. `decodeUtf8Scalars()` reuses the same
state machine and returns the decoded `uint32` scalars, or `std::nullopt` for malformed input.
`appendUtf8Scalar()` covers the four encoding widths; if the caller passes a surrogate or an
out-of-range code point it triggers a `UF_CHECK`, because the function's precondition already states
that the parameter must be a Unicode scalar.

Raw code-unit representation conversions are concentrated in
`modules/core/source/core/text/unsafe/unicode-code-unit.hpp`: `utf8CodeUnitValue`, `utf8CodeUnit`,
and `utf16CodeUnitValue` use `std::bit_cast` to preserve the object representation, avoiding the
ambiguity of `char` signedness, narrowing, and aliasing. Each operation has a local `SAFETY`
argument, and the ordinary UTF-8 algorithm only consumes safe integer values.

### types and time: Establishing a Vocabulary That Cannot Be Confused

`modules/core/source/core/types/integer.hpp` provides `int8` through `uint64`, `intptr`/`uintptr`,
and `intmax`/`uintmax`, so that cross-module width intent appears directly in the interface.

`StrongValue<Tag, Representation>` in `modules/core/source/core/types/strong-value.hpp` generates
mutually incompatible types through different `Tag`s. Construction is explicit and has no default
constructor; two domain values with the same representation cannot implicitly interconvert. A
scalar `value() const&` returns by value, a non-scalar returns by `const&`, and an rvalue
`value()` moves the representation out. `StrongValueHash` provides explicit opt-in hashing.

`StrongId<Tag, Representation = uint64>` in `modules/core/source/core/types/strong-id.hpp` is
merely a `StrongValue` alias that constrains the representation to be unsigned, adding no fictional
ID-lifetime policy. `Generation<Tag, Representation>` provides `initial()`, `fromValue()`,
`value()`, and `next()`; at the maximum value `next()` returns `std::nullopt` and never wraps to
reuse an old generation.

`NonZero<Value>::create()` in `modules/core/source/core/types/non-zero.hpp` returns an object only
when the value is non-zero, and the private constructor guarantees that after a successful
construction the invariant continues to hold. `Flags<Enum>` in
`modules/core/source/core/types/flags.hpp` only accepts an unsigned underlying enum and provides
`bits`, `empty`, `containsAll`, `containsAny`, `insert`, `remove`, `with`, `without`, and the
inter-set `|`/`&`; there is no raw-bit constructor or unbounded complement, avoiding quietly
introducing bits the enum did not declare.

`modules/core/source/core/types/enum-reflection.hpp` explicitly registers enumerators via
`UF_REFLECT_ENUM`. `EnumTraits` holds a static `EnumEntry` array, and consteval validation rejects
an empty set, an empty name, a duplicate value, and a duplicate name; `enumEntries()`,
`enumName()`, and `enumFromName()` support sparse enums and return `std::nullopt` for an unknown
mapping. The label generated by the macro comes from the C++ token, and renaming an enumerator
also changes the label, so it is not a persistent wire contract that needs no versioning.

`MonotonicInstant` in `modules/core/source/core/time/monotonic-time.hpp` wraps
`std::chrono::steady_clock::time_point`. `now()` obtains an in-process monotonic instant,
`fromTimePoint()` provides an injection point for deterministic tests, `checkedAdd()` reports
overflow using checked integer addition, and `saturatingDurationSince()` returns zero for a
reversed instant and `Duration::max()` for a positive difference that cannot be represented. It is
used for timeout, age, and interval; it is not a wall clock, and it is not a serialization type
that can cross processes, cross machines, or be persisted to disk.

## Constraints That Must Remain True

**Fail-closed.** All dangerous boundaries reject before continuing: checked operations return an
empty value instead of wrapping, `tryAt` returns a null pointer on out-of-bounds, `checkedAt` and
a violated contract terminate directly, the UTF-8 validator rejects non-canonical scalar
encodings, and generation exhaustion does not wrap around. If an `Error` does not belong to a
category the upper layer knows, the upper layer may classify conservatively; a concrete example is
that `failureResponse(Error const&)` in `modules/domain/source/domain/error.cpp` returns `Abort`
for an unrecognized detail category. `core` provides the unambiguous representation needed to make
a fail-closed decision, but product policy still remains in the owner module.

**Determinism.** Checked numeric does not rely on UB or implementation-defined overflow; enum
mapping is explicit, compile-time validated, and looked up linearly in registration order; error
context and `toString()` render in append order; and `ScopeExit` cleanup is executed by exactly
the currently armed owner. `MonotonicInstant` avoids wall-clock jumps and provides a test entry
point for constructing a fixed `TimePoint`. This does not mean that `now()` or thread scheduling
is replayable, but rather that it isolates the sources of nondeterminism within the controllable
mechanisms.

**Ownership and lifetime.** `Error`, `ScopeExit`, and `Synchronized` all forbid implicit copying;
the former two transfer sole responsibility through move. `Continue`/`Break` do not allow a
reference payload, checked range access does not accept a temporary owner, and
`Synchronized::withLock` does not allow directly returning a pointer/reference. View interfaces use
`UF_LIFETIME_BOUND` and note the invalidation conditions. The mechanisms preferentially make the
owner appear in the type and the scope, rather than relying on naming to hint at safety.

**Strict-background.** This is not a runtime invariant of `core`. `core` does not touch the
window, the capture API, or input delivery, and cannot judge whether an action landed on a
background target. Its contribution is to let the controller return a typed failure with a
`Result` return type, check timeliness with `MonotonicInstant`, avoid coordinate overflow with
checked numeric, and terminate when an internal invariant is broken; "never fall back to
foreground input" is enforced by `modules/controller` and the composition root. Only by leaving
the policy with the platform owner can `core` remain portable.

## Consumers

Incoming edges point from all consumers toward `core`, and the only outgoing edge is the C++23
standard library; no project module type crosses into the `core` API. What crosses the boundary
are values, standard views, `Result<T>`/`Error`, `MonotonicInstant`, and a small template
vocabulary, not a platform handle or a business object.

A typical collaboration chain is as follows:

1. `domain` uses `StrongId`/`Generation` to establish mutually unconfusable vocabulary such as
   `FrameId`, `TaskRunId`, and `TargetGeneration`, and uses its own error category to pack
   `AutomationErrorKind` into a core `Error`.
2. `vision`, `image`, and `annotation` use checked arithmetic/cast/access to handle size, stride,
   offset, and index, use the UTF-8 and enum helpers for deterministic validation, and send
   failure upward via `Result`.
3. `engine`'s ports and session exchange only platform-independent objects and `Result`, and use
   monotonic time to handle observation age; it does not require `core` to know about capture or
   action policy.
4. The Windows `controller` produces a native `std::error_code` at the platform boundary while
   retaining the domain detail classification; the strict-background judgment belongs to the
   controller.
5. `entry/` composes the modules and calls `toString()` at the user/log boundary or writes
   structured fields into the trace, rather than letting the low-level propagation macros log on
   their own.

This one-way relationship explains why `core` cannot depend on `domain`: if the generic `Error`
directly contained `AutomationErrorKind`, every non-automation use would be polluted by the
product vocabulary, and `domain -> core` would form a reverse edge. The correct way to extend is
for the owner module to build the category, classifier, and more convenient `fail` overloads.

## Tests

`tests/CMakeLists.txt` assembles the following seven files into `test-core`, links
`${PROJECT_NAME}_core`, compiles with C++23 and the repository safety profile, and attaches the
`CI` label:

- `tests/core/test-error.cpp` pins that `Error` is non-copyable, noexcept-movable, that clone
  deep-copies, the detail/context/native rendering, and the success value extraction, verbatim
  failure move, and context appending behavior of the four kinds of `UF_TRY*`.
- `tests/core/test-checked-arithmetic.cpp` pins the rejection boundaries for unsigned
  underflow/overflow, signed min/max, multiply extremes, division by zero, `min / -1`, narrowing,
  fractional, NaN, and Infinity.
- `tests/core/test-strong-types.cpp` uses compile-time assertions to pin that an ID has no
  default/implicit conversion, that different tags are different types, the return form of
  scalar/non-scalar `value()`, and tests hashing, ordering, and generation exhaustion.
- `tests/core/test-capabilities.cpp` pins the variant exhaustive visitor, sparse enum round-trip,
  `ControlFlow` payload, `NonZero`, `Flags`, the `ScopeExit` exactly-once semantics, and
  `Synchronized`'s 4,000 concurrent updates, value initialization, and in-place overload
  resolution.
- `tests/core/test-checked-access.cpp` rejects a temporary vector at compile time and pins
  mutable/const access and out-of-bounds `nullptr` at runtime.
- `tests/core/test-utf8.cpp` covers the four UTF-8 widths as well as isolated continuation,
  overlong, truncated, surrogate, and beyond-maximum scalar.
- `tests/core/test-monotonic-time.cpp` pins checked addition overflow, reversed zeroing, and the
  saturation of an unrepresentable duration difference.

Contract termination and the annotation macros are not disguised as ordinary recoverable paths in
`test-core`; they are further constrained by compilation, downstream use, and the repository
gates. `scripts/check_safety.py` checks that dangerous operations may only reside at the
`unsafe`/`platform`/`ffi`/`external` boundary with a nearby `// SAFETY:`, and
`tests/test-check-safety.py` pins the rules of that gate; `scripts/check_modules.py` pins that
`core` has no dependencies and that the module graph is acyclic. When extending a facility, add the
minimal boundary behavior to the corresponding test file rather than relying only on integration
tests for indirect coverage.

## Extension Rules

The preferred seam for adding a new capability is not to enlarge an existing type but to add a
precise header and let callers include only it. If a non-template implementation is needed, follow
"one implementation file per header"; do not add an aggregate `core.hpp`. Error-vocabulary
extension should happen in the owner module's `std::error_category` and classifier; `Error`
already provides the detail/native/context seam, with no need to add a product enum.

The archived plan `docs/archive/plans/2026-07-20-safe-cpp-core.md` is the historical authority on
the current kernel scope: a generational `SlotMap` and `Signal` are product-level candidates, a
structured `TaskGroup`/bounded `Channel` are considered only after a real asynchronous
orchestration need appears, and serialization, VFS, job system, and the like still remain
explicitly outside the shared core. The current
`docs/plans/2026-07-21-luau-integration-plan.md` declares the script module a consumer of
`core domain` and reuses `Result`/`Status`/`fail`; it does not authorize pushing the Luau runtime,
cancellation policy, or the script error table into `core`.

When evaluating future seams, follow the repository's authoritative `evaluate-core-capability`
process: first find at least two real call sites or a measurable need, confirm that the C++23
standard library cannot solve it just as clearly, and then prove that the new type genuinely
removes invalid state, a lifetime hazard, or duplicated control flow; finally, promote only the
minimal contract that is portable, auditable, and has a short retained test. Once standard
reflection matures, the backend of `EnumTraits` can be replaced while keeping the
`enumName`/`enumFromName` call surface; beyond that, planned product policy should preferentially
enter the module that owns it, rather than penetrating the `core` boundary in the name of a
"generic capability."
