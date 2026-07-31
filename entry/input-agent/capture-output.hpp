#pragma once

#include <core/error/result.hpp>
#include <domain/frame.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace uf::input_agent
{
    [[nodiscard]]
    auto writeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Status;

    [[nodiscard]]
    auto encodeFramePng(
        Frame const& frame,
        std::filesystem::path const& output
    ) -> Result<std::vector<std::byte>>;
}
