#pragma once

#include <string>

namespace uf::task
{
    // Stable bytecode/runtime identity supplied by the vendored Luau boundary.
    [[nodiscard]] auto luauRuntimeVersion() -> std::string;
}
