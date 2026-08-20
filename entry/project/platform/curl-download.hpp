#pragma once

#include <core/error/result.hpp>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace uf::project_entry
{
    [[nodiscard]]
    auto downloadFile(
        std::string_view url,
        std::filesystem::path const& target,
        std::uintmax_t maximumBytes
    ) -> Status;
}
