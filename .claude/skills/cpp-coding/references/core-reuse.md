# Core Reuse

Use the first option that solves the problem:

1. Delete or avoid the requirement if it is speculative.
2. Reuse an existing project module or helper.
3. Use the C++ standard library.
4. Use an already-adopted dependency.
5. Add the smallest new helper only when repeated use justifies it.

Before creating shared facilities, inspect `modules/core/source/core/` and
nearby code. Do not create parallel error, time, cancellation, or logging
abstractions.

When deciding whether a new generic facility belongs in `core`, use the
`evaluate-core-capability` skill before implementation.

The core capability kernel currently provides:

- `core/error/error.hpp`, `result.hpp`, and `contracts.hpp`.
- `core/concurrency/synchronized.hpp` and `core/control/control-flow.hpp`.
- `core/numeric/checked-arithmetic.hpp` and `checked-cast.hpp`.
- `core/safety/annotations.hpp` and `checked-access.hpp`.
- `core/text/utf8.hpp`.
- `core/types/integer.hpp`, `enum-reflection.hpp`, `flags.hpp`, `non-zero.hpp`,
  `strong-value.hpp`, and `strong-id.hpp`.
- `core/time/monotonic-time.hpp`.
- `core/utility/scope-exit.hpp` and `variant-match.hpp`.

Do not add an aggregate `core.hpp`. Include the exact facility needed. Custom
containers, ownership types, callable views, and serialization helpers are not
baseline utilities; add one only when it supplies missing semantics or a clear
ergonomic advantage that the standard library cannot express adequately.

New core facilities should add a capability or ergonomic structure that C++ does
not already express clearly, compose with the standard library, and reduce real
boilerplate or misuse. Do not add restrictions or wrappers solely to imitate Rust.
