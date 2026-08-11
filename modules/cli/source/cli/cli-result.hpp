#pragma once

#include <core/error/error.hpp>
#include <core/types/integer.hpp>

#include <task/task-host.hpp>

#include <string>

namespace uf::cli
{
    enum class ExitCode : uint8
    {
        Success                       = 0,
        Failure                       = 1,
        TargetCompatibilityUnverified = 2,
        Timeout                       = 3,
        Cancelled                     = 4,
    };

    [[nodiscard]]
    auto formatError(Error const& error) -> std::string;

    [[nodiscard]]
    auto exitCodeForError(
        Error const& error,
        bool stopRequested
    ) noexcept -> ExitCode;

    [[nodiscard]]
    auto exitCodeForTaskReport(
        task::TaskRunReport const& report,
        bool stopRequested
    ) noexcept -> ExitCode;
}
