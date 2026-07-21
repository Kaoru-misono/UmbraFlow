# C++ Code Style Guidelines

## Language and files

- Target C++23.
- Use textual `.hpp` and `.cpp` files; do not introduce named C++ modules.
- Filenames use kebab-case.
- Use `#pragma once` in headers.
- The project root namespace is `uf` (the deliberate short form of
  UmbraFlow; the long `umbra_flow` form is not used).

## Naming

- Locals and functions: `camelCase`.
- Data members: `m_camelCase`.
- Pointer and smart-pointer parameters: `p_camelCase`.
- Types: `PascalCase`; interfaces: `IPascalCase`.
- Avoid globals; unavoidable globals/statics use `g_` or `s_`.
- Macros use `UPPER_CASE_WITH_UNDERSCORES` with the project macro prefix
  `UF_` (the deliberate short form of UmbraFlow; the long
  `UMBRA_FLOW_` form is not used).

## Formatting

- Use trailing return types for functions.
- Use Allman braces except concise inline definitions.
- Prefer AAA locals and brace initialization.
- Use east const: `std::string const&`.
- Put `template <...>` with a space before `<`.
- Braces are required for control statements except a one-line `return` or `continue` guard.

### Line wrapping

Follow the April2 wrapping convention exactly. It applies to every wrapped
statement, not only function calls.

- If a statement needs wrapping, the first wrapped element must appear after a
  left delimiter such as `(` or `{`. The left delimiter is the final token on
  its line.
- Indent wrapped content by one level. Each nested wrapped delimiter introduces
  another indentation level.
- Put the matching right delimiter on its own line with reduced indentation,
  aligned with the statement level that owns the left delimiter.
- Apply the same shape to declarations, calls, initializers, macros, and control
  conditions. Do not use an unrelated token or a binary operator as the initial
  wrapping boundary.
- In a wrapped boolean condition, put `if (` or `while (` on its own line, keep
  the first operand unprefixed, and put subsequent `&&` or `||` operators at the
  start of their continuation lines.
- Short arguments may share a wrapped content line when they remain readable;
  the delimiter placement and indentation rules still apply.

Canonical forms:

```cpp
auto createRecord(
    ProjectId projectId,
    RecordOptions const& options
) -> Result<Record>;

auto result = createRecord(
    projectId,
    RecordOptions{
      .m_name = name,
      .m_enabled = true,
    }
);

if (
    request.isValid()
    && (
      request.isLocal()
      || allowRemote
    )
)
{
  process(request);
}
```

Do not use hanging or partially wrapped forms:

```cpp
auto result = createRecord(projectId,
                           options);

auto value = firstValue
    + secondValue;

if (request.isValid()
    && request.isLocal())
{
    process(request);
}
```

### Source text normalization

Match April2's deterministic source normalization:

- Use LF line endings in both the repository and working tree; never CRLF.
- Use spaces only. Tabs normalize to four spaces.
- Do not leave trailing whitespace.
- End every non-empty source file with exactly one newline and no trailing blank
  lines.
- Run `python scripts/fix_format.py --check` to enforce these byte-level rules.

## Class body order

1. Required macros.
2. Constants.
3. Aliases and nested types.
4. Friends.
5. Stored data.
6. Constructors, operators, then ordinary member functions.

Do not interleave stored state and methods.

## Standard library usage

- Use `std::byte` for untyped frame, file, compressed, and readback storage.
- Use `std::uint8_t` only when the byte has numeric meaning, such as a color channel.
- Prefer `std::span` for non-owning contiguous buffers.
- Prefer ranges algorithms, `contains`, `std::erase_if`, structured bindings, and `std::to_underlying` when they improve clarity.
- Use `emplace_back` for every `std::vector` append operation.
- Do not store or return views whose backing lifetime is unclear.

## Ownership

Ownership is expressed by values, members, and function signatures. Do not make
types inherit a common base class solely to participate in an ownership model.

Use this vocabulary consistently:

- `T` is an owned value and is the default.
- `std::unique_ptr<T>` transfers or stores exclusive dynamic ownership.
- `std::shared_ptr<T const>` stores shared immutable ownership. A function that
  retains a share takes the smart pointer by value; a function that only uses
  the object takes `T const&`.
- `std::shared_ptr<T>` is allowed only when the owning API documents and enforces
  synchronization and mutation semantics.
- `std::weak_ptr<T>` observes an existing shared-ownership graph without
  extending lifetime, such as a documented cycle or deferred callback.
- `T&` and `T const&` are required synchronous borrows and never transfer
  ownership.
- `T*` and `T const*` are optional, non-owning observations of one object. A raw
  pointer never represents an array or ownership.
- `std::span` and `std::string_view` are non-owning call-scoped views unless the
  API explicitly exposes and enforces the backing lifetime.

Follow these lifetime rules:

- Prefer containment and standard containers before any dynamic allocation.
- Use RAII for every native handle, registration, mapping, and external resource.
- Do not use raw `new` or `delete` in application code.
- Do not store pointer, reference, iterator, `span`, or `string_view` members by
  default. An exception requires an owner that provably outlives the member, an
  explicit lifetime contract, and focused review.
- Do not return a pointer, reference, iterator, or view into a temporary, a value
  parameter, or storage whose invalidation is not part of the public contract.
- Annotate APIs that intentionally return a borrow tied to a parameter or `this`
  with the project lifetime-bound annotation.
- Move an owner before deriving observers from the destination. Do not retain a
  pointer, reference, iterator, or view obtained from an owner that is later
  moved or reset.
- Synchronous non-escaping callbacks may capture references. Stored or
  asynchronous callbacks capture values, moves, `std::shared_ptr`, or a
  `std::weak_ptr` that is locked at execution time; they do not capture by
  reference or retain a bare `this`.
- `std::enable_shared_from_this` and mutable shared ownership require an explicit
  ownership justification during review.

Construction follows a fixed decision order:

1. Use a public constructor for simple construction that cannot fail.
2. Use a named static factory returning `Result<T>` when establishing the type's
   own invariant can fail and useful error detail is required.
3. Put creation on the owning capability when construction requires that
   capability, such as a device, registry, or platform context.
4. Return `std::unique_ptr<Interface>` for runtime-polymorphic ownership.
5. Return `std::shared_ptr<T const>` only for demonstrated shared immutable
   lifetime.

Avoid two-phase initialization. A successfully constructed object must already
satisfy its invariant.

## Includes

1. Corresponding header.
2. Same-module headers with quotes.
3. Other project modules with angle brackets.
4. Standard library and third-party headers with angle brackets.

Each module's `source/` directory is an include root. For example, another
module includes `<core/error/result.hpp>`, while an implementation includes its
own header with quotes.

## Errors and invariants

- External input, operating-system failure, I/O failure, and unsupported input return `Result<T>` or `Status`.
- `Result<T>` is `std::expected<T, Error>` and `Status` is `Result<void>`; do not
  add a parallel result container.
- Return failures with `fail(...)` for both `Result<T>` and `Status`. Do not use
  templated failure factories or a separate status failure helper.
- Mark every function returning `Result<T>` or `Status` as `[[nodiscard]]`.
- Use `UF_TRY*` for linear propagation and `std::expected` monadic
  operations when they express composition more clearly. Value-extracting
  macros are standalone statements inside a braced block.
- Use `std::optional`, `bool`, a domain enum, `std::variant`, or `ControlFlow`
  instead of `Result<T>` for ordinary absence and normal control flow.
- Broken internal invariants use `UF_ASSERT`.
- Mandatory release-active invariants use `UF_CHECK`.
- Exhausted impossible branches use `UF_UNREACHABLE`.
- Do not log and return at every layer. Add context while propagating and log once at a boundary.
- A local degrade/skip path must log why it degraded.

See `error-handling.md` for construction and propagation examples.

## Comments and debt

- Comments explain why, external constraints, or intentional omissions; never paraphrase code.
- Comments must be English.
- Mark a deliberate shortcut as `TODO(cpp-debt): <shortcut> — ceiling: <X>, upgrade: <Y>`.

## Tests

- Optimize for preserved behavior, not test count or coverage percentage.
- Keep tests concise while covering contracts, boundaries, regressions, and
  compatibility-sensitive behavior.
- Temporary tests used to diagnose or implement a feature must be deleted when
  that feature is complete.
- Do not retain tautological assertions, trivial getter tests, duplicated cases,
  or tests coupled only to private implementation details.
- Prefer table-driven cases when several inputs express one behavior.
