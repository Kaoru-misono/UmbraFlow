#pragma once

#include <core/error/result.hpp>

#include <core/types/integer.hpp>

namespace uf
{
    enum class DpiDeclaration : uint8
    {
        Declared,
        AlreadyDeclared,
    };

    [[nodiscard]] auto ensurePerMonitorAwareV2() -> Result<DpiDeclaration>;
}
