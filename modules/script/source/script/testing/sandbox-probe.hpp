#pragma once

#include <core/error/result.hpp>

#include <string_view>

namespace uf::script::testing
{
    // Test seam: build a fully sandboxed VM that additionally exposes a
    // synthetic, deep-frozen host table as the global `host` (shape
    // { flat = 7, nested = { value = 1 } }), run `source` on a sandboxed task
    // thread, and return its numeric result. It exercises deepFreeze on nested
    // host tables before the real umbra.* tables exist (phase 2). Luau-free so
    // tests may include it; not part of the public Engine surface.
    [[nodiscard]]
    auto runWithFrozenHostTable(
        std::string_view source,
        std::string_view chunkName
    ) -> Result<double>;
}
