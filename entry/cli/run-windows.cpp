#include "run.hpp"

#include "platform/ocr-engine-binding.hpp"
#include "platform/target-binding.hpp"
#include "platform/windows-console-cancellation.hpp"

#include <controller/discovery.hpp>
#include <core/error/result.hpp>

#include <task/task-host.hpp>

#include <utility>

namespace uf::cli
{
    auto runProduct(RunArgs const& args) -> Result<task::TaskRunReport>
    {
        // Held ahead of every later local, so cancellation stays installed for the
        // whole run and is removed last; its token is what lets the generation --
        // and through it the engine and the VM interrupt -- observe a stop.
        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // Before touching the desktop, so a bad project path or a corrupt manifest
        // fails without declaring DPI awareness, enumerating windows, or opening any
        // capture resource.
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

        // Before the target, for the same reason: a model directory that will not
        // build an engine must fail before any window is enumerated.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        // Shared with `explore`, so the two front-ends that bind a live target
        // cannot bind it differently.
        UF_TRY_VALUE(bound, platform::bindTarget(WindowHandle{args.windowHandle}));

        return host.startTask(
            generation,
            args.task,
            task::TaskRunConfig{
                .frameSource             = std::move(bound.frameSource),
                .actionSink              = std::move(bound.actionSink),
                .ocrEngine               = std::move(ocrEngine),
                .liveFingerprint         = bound.liveFingerprint,
                .maximumPixelComparisons = args.budget,
                .recognitionTimeout      = args.recognitionTimeout,
                .maxActionFrameAge       = args.maxFrameAge,
                .maxScriptRuntime        = args.maxRuntime,
                .tracePath               = args.trace,
            }
        );
    }

    // The process-lifetime source outlives the ConsoleCancellation registration, so
    // a Ctrl-C remains observable at the exit-code boundary after the run returns.
    auto runCancellationRequested() noexcept -> bool
    {
        return platform::ConsoleCancellation::stopRequested();
    }
}
