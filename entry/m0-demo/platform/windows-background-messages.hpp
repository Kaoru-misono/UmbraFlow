#pragma once

#include <core/types/integer.hpp>

namespace uf::m0_demo::platform
{
    [[nodiscard]]
    auto isAllowedBackgroundMessage(uint32 message) noexcept -> bool;
}
