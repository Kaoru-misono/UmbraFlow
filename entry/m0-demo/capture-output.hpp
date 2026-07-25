#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>
#include <domain/frame.hpp>

#include <cstddef>
#include <filesystem>
#include <vector>

namespace uf::m0_demo
{
    [[nodiscard]]
    auto indexedOutputPath(
        std::filesystem::path const& output,
        uint32 index,
        uint32 frameCount
    ) -> std::filesystem::path;

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
