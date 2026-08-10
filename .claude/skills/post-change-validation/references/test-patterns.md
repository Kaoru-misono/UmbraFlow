# Test Patterns

Tests use doctest and live under `tests/<module>/`.

```cpp
#include <core/numeric/checked-arithmetic.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <limits>

namespace uf
{
    TEST_CASE("checked multiplication rejects overflow")
    {
        auto constexpr maximum = std::numeric_limits<uint64>::max();
        CHECK_FALSE(checkedMultiply(maximum, uint64{2}).has_value());
    }
}
```

Register sources in `tests/CMakeLists.txt` through `cpp_add_test`, which labels
every test `CI` and gives it a 60 s timeout. That default is correct for a
deterministic offline test. An environment-dependent test must be separate and
explicit, and must overwrite the label after registration —
`set_tests_properties(<name> PROPERTIES LABELS "REAL" TIMEOUT 300)`, as
`test-ocr-real` does — because `ctest -L CI` is the gate and
`set_tests_properties` replaces every property it names rather than adding to
it.

A requirement gate goes through `cpp_add_contract_suite` instead. It builds the
target with `NO_CTEST` and registers one CTest per case: `CI;SCHEMA` when the
case name starts with `schema-`, `CI;CONTRACT` otherwise, and
`CI;CONTRACT-SUITE` on the aggregate. Those two prefixes are the only names it
accepts, and `schema-` means the case only reads a schema file, so
`ctest -L CONTRACT` selects the gates that go red when behaviour is removed.

That function refuses four things, so that no compiled case can run in no gate —
the failure `docs/pitfalls/checks-that-cannot-fail.md` collects, live in this
tree until 2026-08-10. A case name outside the `contract-`/`schema-` vocabulary,
a name absent from `UF_REQUIRED_DOCTEST_CONTRACTS`, a collision with an
already-registered CTest, and `CASES` not matching the `TEST_CASE` names its
sources declare are each a `FATAL_ERROR`: configure dies for the whole tree, not
just this suite, and nobody can build anything until it is fixed. So rename or
delete a case and its `CASES` entry in one edit, and never stop with the two out
of step. (2026-08-10, from `tests/CMakeLists.txt`.)

## Retention rule

Tests should be minimal in code and broad in behavioral coverage. Keep tests that
protect a public contract, a meaningful boundary or invariant, a reproduced bug,
or compatibility-sensitive behavior. Prefer table-driven cases over duplicated
test bodies.

Temporary diagnostic, characterization, and implementation-scaffolding tests are
useful during development, but delete them once the feature is complete. Do not
retain tautologies, trivial accessor checks, or tests of private implementation
details only to increase coverage metrics.
