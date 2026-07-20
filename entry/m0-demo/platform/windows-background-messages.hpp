#pragma once

#include <cstdint>

namespace uf::m0_demo::platform
{
    [[nodiscard]]
    auto isAllowedBackgroundMessage(std::uint32_t message) noexcept -> bool;
}
