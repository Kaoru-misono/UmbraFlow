#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <string>

namespace uf::cli
{
    // Stable process-exit contract for the product CLI. `main` converts this
    // value to int only at the process boundary.
    enum class ExitCode : uint8
    {
        Success                       = 0,
        Failure                       = 1,
        TargetCompatibilityUnverified = 2,
        ActionAbsent                  = 3,
        Timeout                       = 4,
        Cancelled                     = 5,
    };

    // The product of one `run` invocation. When m_actionDelivered is true the
    // click landed and the client coordinates are meaningful; when it is false
    // the page resolved but the action target was absent on it, and the caller
    // maps that to the dedicated absent exit code.
    struct RunReport final
    {
        bool m_actionDelivered{};

        std::string m_pageName{};
        std::string m_actionName{};

        float m_clickClientX{};
        float m_clickClientY{};

        std::string m_tracePath{};
    };

    // Runs the hardcoded smoke flow: bind the target, wait for the page, find
    // the action, and act. Implemented per host: the Windows build performs the
    // full composition; other hosts report the run path as unsupported.
    [[nodiscard]]
    auto runProduct(RunArgs const& args) -> Result<RunReport>;

    // True when a cancellation request (Ctrl-C) reached the process during the
    // run. Implemented per host: the Windows composition installs a console
    // handler backed by a process-lifetime stop source, so this stays readable
    // after the run returns; hosts without that handler report false. The
    // exit-code boundary reads it so a Ctrl-C that surfaced as a capture failure
    // still reports the documented cancellation code.
    [[nodiscard]]
    auto runCancellationRequested() noexcept -> bool;

    // Renders an error for the CLI boundary: kind, message, every context frame,
    // and the originating source location, so one line explains the failure.
    [[nodiscard]]
    auto formatRunError(Error const& error) -> std::string;

    // Maps a failure to its documented process exit code. When stopRequested
    // is true the run always reports cancellation regardless of the underlying
    // kind, so an operator's Ctrl-C intent takes precedence over the failure a
    // blocked step happened to surface.
    [[nodiscard]]
    auto exitCodeForError(
        Error const& error,
        bool stopRequested
    ) noexcept -> ExitCode;
}
