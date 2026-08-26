#pragma once

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <cstddef>
#include <filesystem>
#include <span>

namespace uf::operator_runtime::platform
{
    enum class DurablePublishResult : uint8
    {
        Published,
        AlreadyExists,
    };

    [[nodiscard]]
    auto writeDurableNewFile(
        std::filesystem::path const& path,
        std::span<std::byte const> bytes
    ) -> Status;

    [[nodiscard]]
    auto publishDurableNewFile(
        std::filesystem::path const& staging,
        std::filesystem::path const& destination
    ) -> Result<DurablePublishResult>;
}
