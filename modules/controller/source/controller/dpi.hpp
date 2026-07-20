#pragma once

#include <core/error/result.hpp>

namespace uf
{
    enum class DpiDeclaration
    {
        Declared,
        AlreadyDeclared,
    };

    [[nodiscard]] auto ensurePerMonitorAwareV2() -> Result<DpiDeclaration>;
}
