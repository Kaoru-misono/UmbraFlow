#pragma once

#include <core/error/result.hpp>

#include <cstddef>
#include <filesystem>
#include <span>

namespace uf::workbench::platform
{
    [[nodiscard]]
    auto publishImmutableFile(
        std::filesystem::path const& destination,
        std::span<std::byte const> bytes
    ) -> Status;

    [[nodiscard]]
    auto replaceFileAtomically(
        std::filesystem::path const& destination,
        std::span<std::byte const> bytes
    ) -> Status;
}
