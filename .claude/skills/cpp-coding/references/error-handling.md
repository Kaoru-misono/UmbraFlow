# Error Handling

| Situation | Mechanism |
|---|---|
| Recoverable failure with a value | `Result<T>` |
| Recoverable failure without a value | `Status` |
| Ordinary absence, lookup miss, or optional data | `std::optional<T>` |
| Normal branching or traversal state | `bool`, an enum, a variant, or `ControlFlow` |
| Programmer error or broken invariant | `UF_ASSERT` |
| Release-active mandatory invariant | `UF_CHECK` |
| Impossible control flow | `UF_UNREACHABLE` |
| Safe local degradation | Log once at the site, then skip or clamp |

## Representation and construction

`Result<T>` is an alias for `std::expected<T, Error>`, and `Status` is
`Result<void>`. This is a vocabulary and policy layer, not a replacement result
container. Use normal `std::expected` observers and monadic operations when they
make the flow clearer.

Return a failure with the single `fail(...)` helper. It produces
`std::unexpected<Error>`, so the enclosing function's declared `Result<T>` or
`Status` determines the success type. Do not write `fail<T>`, `failStatus`, or a
parallel result factory.

```cpp
[[nodiscard]]
auto loadProject(std::string_view path) -> Result<Project>
{
    if (path.empty())
    {
        return fail(ErrorCode::InvalidArgument, "project path is empty");
    }

    return Project{path};
}

[[nodiscard]]
auto validateProject(Project const& project) -> Status
{
    if (!project.isValid())
    {
        return fail(ErrorCode::FailedPrecondition, "project is invalid");
    }

    return ok();
}
```

`fail(...)` records its call-site source location by default and accepts an
optional native error code. It only constructs the error; it never logs.

Every function returning `Result<T>` or `Status` must be `[[nodiscard]]`.

## Propagation

Use the propagation form that matches the success value:

```cpp
UF_TRY(validateProject(project));
UF_TRY_CONTEXT(validateProject(project), "opening project");

UF_TRY_VALUE(project, loadProject(path));
UF_TRY_VALUE_CONTEXT(project, loadProject(path), "starting editor");
```

- `UF_TRY` propagates an error when the success value is not needed.
- `UF_TRY_CONTEXT` also appends one meaningful context frame.
- `UF_TRY_VALUE` declares the named value after successful extraction.
- `UF_TRY_VALUE_CONTEXT` extracts the value and adds context on
  failure.
- The value forms expand to multiple statements. Use them only as standalone
  statements inside a braced block.

Use `withContext(result, context)` when returning an already formed result is
clearer than a macro. Direct `and_then`, `or_else`, `transform`, and
`transform_error` use is encouraged when it expresses a composition more
clearly than imperative propagation.

## Context and logging

Low-level functions return errors without logging. Higher layers add context. The CLI or subsystem boundary logs the final error once.

Never turn invalid external input, I/O failure, resource unavailability, or an
unsupported capability into an assertion. Never turn a mandatory internal
invariant into a recoverable result.

Use a stable `ErrorCode` for machine decisions, a concise message for the local
failure, the native code only when an external API supplies one, and context for
each useful subsystem boundary. Do not duplicate the same context at adjacent
layers.

## Do not overuse Result

`Result<T>` represents a recoverable operation that can fail, not every branch
that lacks a value. Do not use it for ordinary cache misses, predicates,
optional configuration, loop termination, state-machine transitions, or
per-frame hot-path signaling. Prefer `std::optional`, `bool`, a domain enum,
`std::variant`, or `ControlFlow` as appropriate.

Do not introduce another `Outcome`, `ErrorOr`, exception wrapper, or custom
expected container. Add domain-specific error detail to `Error` or create a
separate domain value only when real call sites require it.
