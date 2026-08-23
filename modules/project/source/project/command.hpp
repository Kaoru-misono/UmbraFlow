#pragma once

#include <core/error/result.hpp>
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

    // Every verb of the `project` executable, dispatched from its own table.
    // There is nothing for the executable to wire on top: an entry point that
    // intercepted a verb to run something extra before it would be a second
    // place a verb's meaning is written.
    [[nodiscard]]
    auto runProjectCommand(
        std::span<std::string const> raw
    ) -> ProjectExitCode;

    [[nodiscard]] auto projectUsageText() noexcept -> std::string_view;
}
