#pragma once

#include "controller/dpi.hpp"

#include <core/types/integer.hpp>

#include <optional>

namespace uf::controller_detail
{
    inline constexpr auto k_accessDeniedError = uint32{5};

    [[nodiscard]]
    constexpr auto win32Code(uint32 hresult) noexcept -> uint32
    {
        return hresult & 0x0000'FFFFU;
    }

    [[nodiscard]]
    auto classifyDpiResult(
        std::optional<uint32> win32Error,
        bool isPerMonitorAwareV2
    ) -> Result<DpiDeclaration>;
}
