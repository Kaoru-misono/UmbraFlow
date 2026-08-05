#pragma once

#include "args.hpp"
#include "run.hpp"

#include <core/error/result.hpp>
#include <core/types/integer.hpp>

#include <task/task-host.hpp>

namespace uf::cli
{
    // What one `replay` produced, shaped like CheckReport for its reason: a run
    // that never finished has judged nothing and has no verdict at all, which is
    // a different answer from a completed replay that rejected the model.
    struct ReplayReport final
    {
        task::TaskRunReport run{};

        // How many moves the model could not explain. Zero on a completed run is
        // the verdict "accepted"; it is also zero on a run that failed, where
        // `run.failure` is what says so.
        uint64 findings{};
    };

    // Checks one recorded run against the model at `args.project`.
    //
    // The mirror of `checkProduct`. That one measures the screens a model was
    // authored on and this reads a run the model drove, so between them the two
    // evidence libraries make "recognise", "permit" and "change" falsifiable
    // (docs/plans/2026-08-04-state-layer-and-policy-slots.md 4.2).
    //
    // IT OPENS NO FRAME. Everything it judges was measured when the run
    // happened, so the frame source it binds refuses every capture and the
    // action sink refuses every input: a replay that observed or delivered
    // anything would be measuring a screen that is not the one the run saw.
    //
    // TWO REFUSALS HAPPEN HERE AND NOT IN THE LUAU CHECKER, because neither is
    // decidable there:
    //
    //   * A stream whose front end delivers no input is not a run. `umbra-flow
    //     check` resolves every page it cares about against one frame, so its
    //     resolutions are a sweep and not a walk; read as a walk they would
    //     report a task that stood on dozens of pages and delivered nothing. The
    //     exclusion is by front end, a closed enum the host stamps on every
    //     line, and never by task name, which a project can spell any way.
    //
    //   * A stream recorded against another page model cannot be judged by this
    //     one. `run.started` carries the model's content hash and the host reads
    //     the file's own, so the two are compared before a VM boots -- and they
    //     have to be compared here, because the hash never reaches the script
    //     layer at all.
    //
    // The routine writes the verdict to standard output as JSON lines in
    // `replay.groups` order: one summary object, then one line per finding, then
    // one per page transition.
    //
    // A failure here is a replay that could not be performed.
    [[nodiscard]]
    auto replayProduct(ReplayArgs const& args) -> Result<ReplayReport>;

    // The process exit code for a completed replay, on `exitCodeForCheck`'s
    // rules: a failed run defers to that failure, and a completed one reports
    // success only when the model explained every move.
    [[nodiscard]]
    auto exitCodeForReplay(ReplayReport const& report) noexcept -> ExitCode;
}
