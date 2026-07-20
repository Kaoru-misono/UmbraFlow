#pragma once

#include "controller/dpi.hpp"

#include <cstdint>
#include <optional>

namespace uf::controller_detail
{
    inline constexpr auto accessDeniedError = std::uint32_t{5};

    [[nodiscard]]
    constexpr auto win32Code(std::uint32_t hresult) noexcept -> std::uint32_t
    {
        return hresult & 0x0000'FFFFU;
    }

    [[nodiscard]]
    auto classifyDpiResult(
        std::optional<std::uint32_t> win32Error,
        bool isPerMonitorAwareV2
    ) -> Result<DpiDeclaration>;
}
