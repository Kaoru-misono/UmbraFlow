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
- `core/numeric/checked-arithmetic.hpp` and `checked-cast.hpp`.
- `core/safety/annotations.hpp` and `checked-access.hpp`.
- `core/text/utf8.hpp` and `json-text.hpp`.
- `core/types/integer.hpp`, `enum-reflection.hpp`, `strong-value.hpp`, and
  `strong-id.hpp`.
- `core/time/monotonic-time.hpp` and `poll-sleep.hpp`.
- `core/utility/scope-exit.hpp` and `variant-match.hpp`.

A facility belongs on that list only while something outside `core` calls it.
`synchronized.hpp`, `flags.hpp`, `non-zero.hpp`, and `control-flow.hpp` were
removed on 2026-08-11 after 391 commits with no production caller. Each was
rejected on its own grounds rather than on the caller count alone; the four
rulings are recorded in the `evaluate-core-capability` skill's
[`capability-kernel.md`](../../evaluate-core-capability/references/capability-kernel.md).
Do not reintroduce a facility here without a call site.

Do not add an aggregate `core.hpp`. Include the exact facility needed. Custom
containers, ownership types, callable views, and serialization helpers are not
baseline utilities; add one only when it supplies missing semantics or a clear
ergonomic advantage that the standard library cannot express adequately.

New core facilities should add a capability or ergonomic structure that C++ does
not already express clearly, compose with the standard library, and reduce real
boilerplate or misuse. Do not add restrictions or wrappers solely to imitate Rust.
