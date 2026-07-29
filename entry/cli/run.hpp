#pragma once

#include "args.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <task/task-host.hpp>

#include <string>

namespace uf::cli
{
    // Stable process-exit contract for the product CLI. `main` converts this
    // value to int only at the process boundary.
    //
    // 3 is deliberately absent. It was ActionAbsent, reported by the removed
    // single-step smoke path when a page resolved but its action target was not
    // on screen; a task script decides what an absent target means for itself,
    // so nothing produces that code any more. The value is left unused rather
    // than reassigned, because a script or operator reading an old 3 must never
    // be told it meant something else.
    enum class ExitCode : uint8
    {
        Success                       = 0,
        Failure                       = 1,
        TargetCompatibilityUnverified = 2,
        Timeout                       = 4,
        Cancelled                     = 5,
    };

    // Runs one project-owned task through task::TaskHost and reports how it
    // ended. Implemented per host: the Windows build binds a live target and
    // performs the full composition; other hosts report the run path as
    // unsupported. A failure here is a run that never started -- the report
    // carries every way a started run can end.
    [[nodiscard]]
    auto runProduct(RunArgs const& args) -> Result<task::TaskRunReport>;

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

    // Maps a completed run's report to its process exit code. This is the single
    // definition of that mapping: a report with no failure is Success, and every
    // other report defers to exitCodeForError over the failure that ended it, so
    // a started run and a run that never started report the same kind the same
    // way.
    [[nodiscard]]
    auto exitCodeForReport(
        task::TaskRunReport const& report,
        bool stopRequested
    ) noexcept -> ExitCode;
}
