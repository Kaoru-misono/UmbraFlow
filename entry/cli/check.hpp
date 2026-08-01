#pragma once

#include "args.hpp"
#include "run.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // What one `check` produced: how the run itself ended, and what the matrix
    // found.
    //
    // The two are separate because they fail for unrelated reasons. A run that
    // never finished -- a search that ran out of budget, a screen whose PNG is
    // not there, a cancelled generation -- has measured nothing and has no
    // verdict at all, which is a different answer from a completed matrix that
    // rejected the model.
    struct CheckReport final
    {
        task::TaskRunReport run{};

        // How many things the matrix found wrong. Zero on a completed run is the
        // verdict "accepted"; it is also zero on a run that failed, where
        // `run.failure` is what says so.
        uint64 findings{};
    };

    // Runs the falsification matrix over the project at `args.project` and
    // reports it.
    //
    // It binds no target and touches no desktop: the frames come from
    // <project>/assets/screens through cli::FileFrameSource, so this is the one
    // product verb that composes fully on every host. The matrix itself is
    // trusted framework Luau run through task::TaskHost::runFrameworkRoutine --
    // not a project task, because the routine is the host's own and a project
    // must not be able to supply, replace or shadow the thing that judges it.
    //
    // The verdict is written to standard output as JSON lines by the routine
    // itself, one summary object, one object per finding, one per separation
    // measurement and one per cell. A failure here is a check that could not be
    // performed; a report is a check that was.
    [[nodiscard]]
    auto checkProduct(CheckArgs const& args) -> Result<CheckReport>;

    // The process exit code for a completed check. A run that ended in a failure
    // defers to that failure exactly as a task run does, so the two subcommands
    // report one kind one way; a run that completed reports success only when
    // the matrix accepted the model.
    [[nodiscard]]
    auto exitCodeForCheck(CheckReport const& report) noexcept -> ExitCode;
}
