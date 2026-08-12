#pragma once

#include <core/types/integer.hpp>

#include <span>
#include <string>
#include <string_view>

namespace uf::project
{
    enum class ProjectExitCode : uint8
    {
        Success = 0,
        Failure = 1,
    };

    [[nodiscard]]
    auto runProjectCommand(
        std::span<std::string const> raw
    ) -> ProjectExitCode;

    [[nodiscard]] auto projectUsageText() noexcept -> std::string_view;
}
