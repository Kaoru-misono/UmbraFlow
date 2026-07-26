# C++ Code Style Guidelines

**Nothing in this document is checked mechanically unless its section says so.**

A section that opens with a command is enforced by that command: run it instead
of verifying the section by reading. A marker states exactly what its tool
covers, because a rule that only *looks* enforced is worse than one openly left
to the reader.

Two enforcement layers exist. The `scripts/*.py` gates run on every host. The
required `clang-analysis` CI job builds the `linux-analysis` preset with
clang-tidy under `WarningsAsErrors: '*'`, which adds
`cppcoreguidelines-pro-type-member-init`, `bugprone-dangling-handle`,
`bugprone-use-after-move`, `cppcoreguidelines-owning-memory`, and the bounds and
cast checks in `.clang-tidy`. That job is the only thing checking parts of
`## Ownership`, and it does not run locally on Windows.

Everything else — most of this document, including all of line wrapping, type
aliases, parameter direction, and class body order — has no automated check on
either layer. There this text is the only enforcement and a violation reaches
review unnoticed. Budget attention accordingly.

## Language and files

- Target C++23.
- Use textual `.hpp` and `.cpp` files; do not introduce named C++ modules.
- Filenames use kebab-case.
- Use `#pragma once` in headers.
- The project root namespace is `uf` (the deliberate short form of
  UmbraFlow; the long `umbra_flow` form is not used).

## Naming

- Locals and functions: `camelCase`.
- Public data members — the members of a `struct` body, which are public by
  default and which this project uses for aggregates — are plain `camelCase`
  with no prefix.
- Private data members — the members of a `class` body — are `m_camelCase`. A
  member left private under an explicit `private:` in a `struct`, or made public
  under `public:` in a `class`, follows its actual access: `m_` when private,
  no prefix when public.
- Pointer and smart-pointer parameters: `p_camelCase`.
- Types: `PascalCase`; interfaces: `IPascalCase`.
- Avoid non-local state. Named value constants with static storage duration use
  `k_`. Mutable namespace-scope state uses `g_`. Class or function static state
  whose meaning is identity, a cache, a singleton, an initialization guard, or
  mutable storage uses `s_`. Automatic locals remain `camelCase` even when they
  are `const` or `constexpr`.
- Macros use `UPPER_CASE_WITH_UNDERSCORES` with the project macro prefix
  `UF_` (the deliberate short form of UmbraFlow; the long
  `UMBRA_FLOW_` form is not used).

## Type aliases

Two different declarations are called an alias. They carry different costs, and
a rule for one does not govern the other. One question decides which: can code
outside this translation unit name it? That is a fact about the code rather
than a judgment, and it classifies every alias.

- A vocabulary alias can be named from outside. It sits at namespace scope in a
  header, or in the public or protected section of a type reachable from one.
  It enters an API, so every reader must learn it.
- A local abbreviation cannot. It is private, function-local, or declared in an
  anonymous namespace or any other internal-linkage context in a `.cpp` file.
  It names a spelling and disappears with its translation unit.

Rules:

- A vocabulary alias must add meaning the expansion does not carry: a sum type,
  a callable protocol, a strong-identity wrapper, or a type that varies with a
  template parameter. Length alone is a trigger for the question, never the
  justification for the answer. A nested typedef that merely requalifies one
  fixed type adds no meaning and does not qualify. Declare a namespace-scope
  vocabulary alias in the header that defines the type it abbreviates,
  immediately after that definition and in the same namespace, so the two cannot
  drift apart; a member alias belongs in its class body per `## Class body
  order`.
- A local abbreviation does not have to add meaning; it has to avoid removing
  any. It is warranted when the same long spelling appears more than once in its
  scope, or when one long spelling sets the alignment column for a member block
  that no semantic boundary can split. Declare it in the smallest scope that
  uses it.
- Brevity on its own is not a reason. An abbreviation that does not lower its
  block's alignment column is not warranted by alignment, because shortening the
  longest declarator can hand the column to the next one and leave the block
  worse than it started. Compare the block before and after.
- Neither kind may alias away `std::optional`, `std::span`, `std::string_view`,
  a raw pointer, or a standard container. Optionality, borrow scope, and
  container shape are contract the reader must see.
- Neither kind may hide, weaken, or reassign an ownership category. An alias
  that abbreviates an owning type must carry that ownership in its own name; see
  `## Ownership` for the vocabulary it must preserve. Note that needing such a
  name does not by itself make the alias warranted — the tests above still
  apply.

```cpp
// Vocabulary: enters the API, and a sum type is meaning the expansion lacks.
using PageAttemptResult = std::variant<PageOutcome, PageRecognitionStop>;

class CaptureDevice final
{
    // Local abbreviation: private, repeated, and dies with the class.
    using DeviceComPtr = winrt::com_ptr<ID3D11Device>;

    DeviceComPtr m_device;
    DeviceComPtr m_fallback;
};
```

```cpp
// Avoid: public in a header type, so it is vocabulary, but it only
// requalifies one fixed type.
struct Args final
{
    using Duration = MonotonicInstant::Duration;
};

// Avoid: hides optionality and container shape.
using Ids = std::vector<RecognizerId>;
using MaybeOffset = std::optional<TemplateOffset>;

// Avoid: a borrow word naming an owner.
using FrameRef = std::shared_ptr<Frame const>;
```

## Type placement

The alias question extends to types: a name must earn the scope it is declared
in. One question decides where a header-level type belongs: does anything name
it other than through one class's operations?

- A type that exists only as a parameter or result of one class's operations —
  a spec consumed by one `place` call, a result only that class returns, the
  callable protocol only its worker runs — is declared inside that class.
  Callers then write `EditPage::NewRegionSpec{...}`, which names the owner at
  the point of use, and the name is unreachable without naming the class.
- A type with more than one unrelated consumer, or with an identity of its own
  (an entity class, a handle, a value that crosses layers), stays at namespace
  scope. Nesting it inside one consumer would misfile it.
- The vocabulary of a free-function layer stays at namespace scope: a free
  function cannot own a nested type, and moving its specs into an incidental
  class would invent an owner that does not exist.
- Dependency direction wins over conceptual ownership. A shared key or row type
  nests in the lowest type that needs it, and higher layers re-expose it as a
  member alias (`using MemberId = PageView::MemberId;`) rather than reversing
  an include edge to reach the "nicer" owner.

The cost is real and accepted: a nested type cannot be forward-declared, so
anyone who names it includes the full header. That is acceptable precisely
because a single-owner type's callers already include that header to call the
operation; if that stops being true, the type has grown a second consumer and
belongs at namespace scope again.

## Namespaces

- Nest every file-local anonymous namespace inside the narrowest owning `uf`
  namespace, such as `uf`, `uf::controller_detail`, or `uf::m0_demo`.
- Never place an anonymous namespace directly at global scope. Keeping internal
  declarations inside their owning project namespace makes project vocabulary,
  including integer aliases, available without redundant `uf::` qualification.

```cpp
namespace uf::m0_demo
{
    namespace
    {
        constexpr auto k_retryLimit = uint32{3};
    }
}
```

## Enums

- Use only scoped enums: `enum class` or `enum struct`.
- Declare an explicit project integer underlying type for every enum, such as
  `uint8`, `uint16`, `uint32`, or `uint64` from
  `<core/types/integer.hpp>`.
- Choose the smallest type that represents the full domain. Use a signed
  fixed-width type only when the enum requires negative values.
- Do not use unscoped enums or rely on the implementation-defined default
  underlying type.

```cpp
enum class ConnectionState : uint8
{
    Disconnected,
    Connected,
};
```

## Concepts and templates

- Constrain every public or reusable function and class template at its
  declaration when it accepts a narrower domain than all possible types.
- Prefer standard concepts such as `std::integral`, `std::same_as`, and
  `std::invocable` when they express the complete contract.
- Define a named project concept only for a reused constraint or a meaningful
  domain abstraction. Keep it in the narrowest owning namespace.
- Express constraints with a constrained template parameter or a `requires`
  clause. Do not hide an interface constraint in a function-body
  `static_assert` or legacy SFINAE.
- An implementation-only template in a `.cpp` file may remain unconstrained
  when its instantiation set is closed and locally controlled.
- Do not add decorative or vacuous concepts that fail to describe operations
  actually required by the implementation.

## Formatting

- Use trailing return types for functions.
- Use Allman braces except concise inline definitions.
- Prefer AAA locals and brace initialization.
- Use east const: `std::string const&`.
- Put `template <...>` with a space before `<`.
- Braces are required for control statements except a one-line `return` or
  `continue` guard.

### Alignment

> Checked by `python scripts/check_cpp_format.py`, which is the CI gate, and
> repaired by the same script with `--fix`. It is a conservative recognizer, not
> a formatter: it skips pointer and reference declarators, bitfields, function
> declarations, macros, preprocessor lines, templates containing commas, local
> classes, and every wrapped statement. A skipped line is dropped from its block
> rather than merely unchecked, so the column the tool computes can differ from
> the column the first rule below requires. Where a block boundary belongs is a
> judgment nothing checks; see the last rule.

- In each contiguous block of two or more single-line data member declarations,
  align the member identifiers at one column, using spaces after the type or
  declarator. The longest type or declarator in the block sets the column.
- In each contiguous block of two or more single-line assignments, `=`-based
  initializers, or adjacent designated-initializer entries, align the assignment
  operators at one column. The longest left-hand side sets the column.
- A blank line starts a new alignment block. Do not align semicolons or
  initializer braces, do not force a wrapped declaration into this form, and do
  not pad a wrapped assignment merely to join a block.
- When a single long member type sets the column for its block and no alias is
  warranted, start a new alignment block with a blank line at an existing
  semantic boundary. Do not split a coherent group of members merely to satisfy
  the formatter; leaving the padding is an acceptable outcome.

```cpp
// A class: its data members are private, so they keep the m_ prefix.
class SourceRecord final
{
    SourceId    m_id;
    ContentHash m_contentHash;
    std::string m_relativePath{};
    uint32      m_revision{};

    explicit SourceRecord(
        SourceId id,
        ContentHash contentHash
    )
        : m_id{std::move(id)}
        , m_contentHash{std::move(contentHash)}
    {
    }
};

m_id          = id;
m_contentHash = contentHash;

// A Spec aggregate: its members are public, so they carry no prefix.
auto source = AuthoringSourceSpec{
    .id          = id,
    .contentHash = contentHash,
    .fingerprint = fingerprint,
    .provenance  = provenance,
};
```

### Line wrapping

> No formatter reproduces this convention. It was measured against clang-format
> and every configuration diverged, so no tool will ever catch a violation here.

Follow the repository wrapping convention exactly. It applies to every wrapped
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
        .name    = name,
        .enabled = true,
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

> Enforced and repaired by `python scripts/fix_format.py`. Run it rather than
> checking these by reading.

LF line endings in both the repository and working tree, spaces only with tabs
normalized to four, no trailing whitespace, and exactly one closing newline with
no trailing blank lines.

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
- Use `uint8` only when the byte has numeric meaning, such as a color channel.
- Use the integer aliases from `<core/types/integer.hpp>` instead of spelling
  fixed-width, pointer-width, or maximum-width integer types through `std`.
- Include `<core/types/integer.hpp>` directly in each file that uses those
  aliases; do not rely on a transitive include or a compiler forced include.
- Prefer `std::span` for non-owning contiguous buffers.
- Prefer ranges algorithms, `contains`, `std::erase_if`, structured bindings,
  and `std::to_underlying` when they improve clarity.
- Use `emplace_back` for every `std::vector` append operation.

## Ownership

> `python scripts/check_safety.py` rejects `.detach(`, `std::unreachable`, and
> the ADR-011 Win32 input APIs *everywhere, including inside a boundary*. It
> rejects raw `new`/`delete`, `malloc`/`free`, `reinterpret_cast`, and
> `const_cast` outside an `unsafe/`, `platform/`, or `ffi/` directory, and
> inside one still requires a `// SAFETY:` comment within the preceding three
> lines. The `clang-analysis` CI job additionally covers part of the lifetime
> rules through `bugprone-dangling-handle`, `bugprone-use-after-move`, and
> `cppcoreguidelines-owning-memory`. Nothing checks the ownership vocabulary or
> the construction order, which are the substance of this section.

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
- An alias that abbreviates an owning type keeps the ownership category in its
  name and never introduces a new one. `...Ref`, `...Ptr`, and `...Handle` are
  reserved for the borrow and observation rows above and must not name an owner.
  Do not erase `const` from `std::shared_ptr<T const>`. If `## Type aliases`
  warrants an alias for a shared owner, its name keeps both facts, as in
  `SharedFrameBuffer`. This governs how such an alias is named, not whether to
  introduce one.

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

### Data member initialization

> Partly enforced by `python scripts/check_safety.py`, which reports a dead
> in-class initializer, and a missing one when the member's type is on its
> allowlist of default-constructible types *and* a construction path visibly
> leaves it default-initialized. It is a conservative recognizer and stays
> silent on any class it cannot parse with confidence, including class
> templates and classes whose name appears more than once. The sentinel,
> ambiguity, and aggregate rules below are not checked at all.
> `cppcoreguidelines-pro-type-member-init` in the `clang-analysis` CI job
> covers the indeterminate cases this misses.

Avoid two-phase initialization: a successfully constructed object must already
satisfy its invariant. That applies at member granularity, and it does not
depend on how callers construct the type. `Widget value;` must leave no member
indeterminate, because correctness belongs to the type rather than to the
discipline of every construction site.

- Every stored data member carries an in-class brace initializer unless it
  cannot.
- A member omits the in-class initializer only when the value must come from
  construction: a required domain value the type refuses to default, or a type
  with no default constructor. Every constructor then brace-initializes it in
  its member-initializer list. Do not invent an invalid sentinel or use
  `std::optional` solely to make `{}` possible.
- Do not give a member both an in-class initializer and a member-initializer
  list entry in every constructor. The constructor wins, so the in-class
  initializer is dead and falsely advertises a valid default state.
- State the intended value when an empty initializer would be ambiguous.
- Aggregate spec and transport types keep designated initialization at each
  construction site, and their members still carry in-class initializers so a
  default-initialized aggregate is never indeterminate. Supply every member
  that carries no in-class initializer explicitly at each construction site.
  Do not add a constructor solely to defeat useful aggregate syntax. An empty
  `Type{}` supplies no member and therefore satisfies nothing on its own.

## Parameter direction and return values

- Parameters are inputs by default. Do not use a reference, pointer, view, span,
  buffer, or callback parameter as an output channel for a computed result.
- Return one computed value directly. When a function produces several values,
  return a named result type. Use `Result<T>` when the operation can fail
  recoverably.
- A mutable parameter is allowed only when mutating caller-owned state is the
  function's primary operation, an external API/ABI or callback signature
  requires it, or measurement demonstrates that returning the value violates a
  documented hot-path requirement. Secondary results and counters are output
  values, not genuine in-out state.
- Make every non-obvious exception explicit at the declaration and document why
  a normal return value is unsuitable. Convenience and speculative performance
  are not sufficient justification.
- Pass small scalar inputs such as `bool`, `int32`, and `uint64` by value. A
  mutable scalar reference requires the same exception review as any other
  output or in-out parameter.

### Description and ownership parameters

- Types ending in `Spec`, `Desc`, or `Descriptor` describe the input used to
  create another object. Non-trivial `Config` and `Options` types normally have
  the same read-only input semantics. Their names do not imply ownership
  transfer even when they contain owning strings, containers, or variants.
- Pass a non-trivial description by `T const&` when a function only validates
  it or copies its fields into stored members. Do not move from it, add an
  rvalue-reference overload only to reuse construction logic, or make its
  behavior depend on whether the caller supplied an lvalue or rvalue.
- When validation requires sorting, deduplication, default insertion, or other
  normalization, first copy the description into a clearly named local such as
  `normalizedSpec`, then modify that local. An internal constructor may take
  the normalized owned value by value and move its members into final storage.
- Pass small trivially copyable descriptions by value when that is clearer and
  no ownership or moved-from state is involved.
- Pass an owning payload by value when the function is a copy-in/move-in
  ownership boundary. Use a concrete `T&&` only for an internal sink that must
  consume an expiring object and intentionally rejects lvalues.
- Reserve forwarding references for generic template boundaries. Forward them
  with `std::forward`; a concrete `WidgetSpec&&` is an rvalue reference, not a
  forwarding reference.

```cpp
[[nodiscard]]
auto Widget::create(WidgetSpec const& spec) -> Result<Widget>
{
    auto normalizedSpec = spec;
    std::ranges::sort(normalizedSpec.ids);

    UF_TRY(validate(normalizedSpec));
    return Widget{std::move(normalizedSpec)};
}
```

```cpp
struct ParseOutcome
{
    Record      record;
    std::size_t consumed{};
};

[[nodiscard]]
auto parseRecord(std::string_view text) -> Result<ParseOutcome>;

// Avoid: results are hidden in the parameter list.
auto parseRecord(
    std::string_view text,
    Record& record,
    std::size_t& consumed
) -> Status;
```

## Includes

> `python scripts/check_modules.py` parses `modules/*/manifest.txt` and rejects
> duplicate module names, a missing `source/` directory, a `core` that declares
> dependencies, self-dependency, and cycles. It never reads a source file, so
> neither the include order below nor an undeclared cross-module include is
> checked by it; only the build's include directories constrain those.

1. Corresponding header.
2. Same-module headers with quotes.
3. Other project modules with angle brackets.
4. Standard library and third-party headers with angle brackets.

Each module's `source/` directory is an include root. For example, another
module includes `<core/error/result.hpp>`, while an implementation includes its
own header with quotes.

## Errors and invariants

> `[[nodiscard]]` on functions returning `Result<T>`, `Status`, or
> `std::optional` is enforced in headers by `python scripts/check_safety.py`,
> which also rejects `std::unreachable` and explicit `throw` in `core`. The
> remaining rules are not checked.

- External input, operating-system failure, I/O failure, and unsupported input
  return `Result<T>` or `Status`.
- `Result<T>` is `std::expected<T, Error>` and `Status` is `Result<void>`; do not
  add a parallel result container.
- Return failures with `fail(...)` for both `Result<T>` and `Status`. Do not use
  templated failure factories or a separate status failure helper.
- Mark every function returning `Result<T>`, `Status`, or `std::optional` as
  `[[nodiscard]]`.
- Use `UF_TRY*` for linear propagation and `std::expected` monadic
  operations when they express composition more clearly. Value-extracting
  macros are standalone statements inside a braced block.
- Use `std::optional`, `bool`, a domain enum, `std::variant`, or `ControlFlow`
  instead of `Result<T>` for ordinary absence and normal control flow.
- Broken internal invariants use `UF_ASSERT`.
- Mandatory release-active invariants use `UF_CHECK`.
- Exhausted impossible branches use `UF_UNREACHABLE`.
- Do not log and return at every layer. Add context while propagating and log
  once at a boundary.
- A local degrade/skip path must log why it degraded.

See `error-handling.md` for construction and propagation examples.

## Comments and debt

- Comments explain why, external constraints, or intentional omissions; never
  paraphrase code.
- Comments must be English.
- Mark a deliberate shortcut as `TODO(cpp-debt): <shortcut> — ceiling: <X>,
  upgrade: <Y>`.

## Tests

- Optimize for preserved behavior, not test count or coverage percentage.
- Keep tests concise while covering contracts, boundaries, regressions, and
  compatibility-sensitive behavior.
- Temporary tests used to diagnose or implement a feature must be deleted when
  that feature is complete.
- Do not retain tautological assertions, trivial getter tests, duplicated cases,
  or tests coupled only to private implementation details.
- Prefer table-driven cases when several inputs express one behavior.
