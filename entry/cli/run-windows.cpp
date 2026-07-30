#include "run.hpp"

#include "platform/target-binding.hpp"
#include "platform/windows-console-cancellation.hpp"

#include <core/error/result.hpp>

#include <task/task-host.hpp>

#include <utility>

namespace uf::cli
{
    auto runProduct(RunArgs const& args) -> Result<task::TaskRunReport>
    {
        // Install Ctrl-C cancellation before anything can block. The registration
        // is held here, ahead of every later local, so it stays installed for the
        // whole run and is removed last; its token is what lets the generation --
        // and through it the engine and the VM interrupt -- observe a stop.
        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // Load the project before touching the desktop, so a bad project path or
        // a corrupt manifest fails without declaring DPI awareness, enumerating
        // windows, or opening any capture resource.
        auto host = task::TaskHost{};
        UF_TRY_VALUE(
            generation,
            host.loadProject(
                args.project,
                task::TaskHostConfig{
                    .externalCancellation = cancellation.token(),
                }
            )
        );

        // The target binding is platform/target-binding.hpp's, shared with `drive`, so
        // the two front-ends cannot come to bind a target differently.
        UF_TRY_VALUE(bound, platform::bindTarget(args.selector));

        return host.startTask(
            generation,
            args.task,
            task::TaskRunConfig{
                .frameSource             = std::move(bound.frameSource),
                .actionSink              = std::move(bound.actionSink),
                .liveFingerprint         = bound.liveFingerprint,
                .maximumPixelComparisons = args.budget,
                .recognitionTimeout      = args.recognitionTimeout,
                .maxActionFrameAge       = args.maxFrameAge,
                .tracePath               = args.trace,
            }
        );
    }

    // Reads the process-lifetime console cancellation source, which outlives the
    // ConsoleCancellation registration installed during a run, so a Ctrl-C remains
    // observable at the exit-code boundary after the run has returned.
    auto runCancellationRequested() noexcept -> bool
    {
        return platform::ConsoleCancellation::stopRequested();
    }
}
