#pragma once

#include <filesystem>

namespace uf::m0_demo::platform
{
    [[nodiscard]]
    auto pathsEqualOrdinal(
        std::filesystem::path const& left,
        std::filesystem::path const& right
    ) noexcept -> bool;
}
