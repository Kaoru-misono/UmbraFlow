#pragma once

#include "args.hpp"
#include "run.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // What one `check` produced: how the run itself ended, and what the matrix
    // found. Separate because a run that never finished has measured nothing and
    // has no verdict at all, which is a different answer from a completed matrix
    // that rejected the model.
    struct CheckReport final
    {
        task::TaskRunReport run{};

        // How many things the matrix found wrong. Zero on a completed run is the
        // verdict "accepted"; it is also zero on a run that failed, where
        // `run.failure` is what says so.
        uint64 findings{};
    };

    // Runs the falsification matrix over the project at `args.project`.
    //
    // It binds no target and touches no desktop: the frames come from
    // <project>/assets/screens through cli::FileFrameSource, so this is the one
    // product verb that composes fully on every host. The matrix is trusted
    // framework Luau run through task::TaskHost::runFrameworkRoutine, not a
    // project task, because a project must not be able to supply, replace or
    // shadow the thing that judges it.
    //
    // The routine writes the verdict to standard output as JSON lines, in
    // `regress.groups` order: one summary object, then one line per finding, per
    // separation measurement, per anchor subset, per page resolved on a screen,
    // per declared page, and per cell.
    //
    // Two of those seven blocks depend on the run rather than on the file. The
    // subset rows are decided by the file alone and are written whether or not
    // this run swept; the two co-resolution blocks -- `resolution` and
    // `page_coverage` -- exist only when it did (CheckArgs::sweepPages), because
    // a run that offered no page to any screen would otherwise report zero
    // resolutions for every page.
    //
    // A failure here is a check that could not be performed.
    [[nodiscard]]
    auto checkProduct(CheckArgs const& args) -> Result<CheckReport>;

    // The process exit code for a completed check. A failed run defers to that
    // failure exactly as a task run does; a completed run reports success only
    // when the matrix accepted the model.
    [[nodiscard]]
    auto exitCodeForCheck(CheckReport const& report) noexcept -> ExitCode;
}
