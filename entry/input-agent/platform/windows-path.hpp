#pragma once

#include <filesystem>

namespace uf::input_agent::platform
{
    [[nodiscard]]
    auto pathsEqualOrdinal(
        std::filesystem::path const& left,
        std::filesystem::path const& right
    ) noexcept -> bool;
}
