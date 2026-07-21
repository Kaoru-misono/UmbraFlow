# Test Patterns

Tests use doctest and live under `tests/<module>/`.

```cpp
#include <core/numeric/checked-arithmetic.hpp>
#include <core/types/integer.hpp>

#include <doctest/doctest.h>

#include <limits>

TEST_CASE("checked multiplication rejects overflow")
{
    auto constexpr maximum = std::numeric_limits<uf::uint64>::max();
    CHECK_FALSE(
        uf::checkedMultiply(maximum, uf::uint64{2}).has_value()
    );
}
```

Register sources in `tests/CMakeLists.txt` through `cpp_add_test`. Use label
`CI` for deterministic offline tests. Environment-dependent tests must be
separate, explicit, and excluded from the default CI label.

## Retention rule

Tests should be minimal in code and broad in behavioral coverage. Keep tests that
protect a public contract, a meaningful boundary or invariant, a reproduced bug,
or compatibility-sensitive behavior. Prefer table-driven cases over duplicated
test bodies.

Temporary diagnostic, characterization, and implementation-scaffolding tests are
useful during development, but delete them once the feature is complete. Do not
retain tautologies, trivial accessor checks, or tests of private implementation
details only to increase coverage metrics.
