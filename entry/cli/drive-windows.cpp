#include "drive.hpp"

#include "platform/ocr-engine-binding.hpp"
#include "platform/target-binding.hpp"
#include "platform/windows-console-cancellation.hpp"

#include <core/error/result.hpp>

#include <task/operator-session.hpp>
#include <task/task-host.hpp>

#include <utility>

namespace uf::cli
{
    auto driveProduct(DriveArgs const& args) -> Result<task::TaskRunReport>
    {
        // The IPC paths are checked first, before anything observable exists: a
        // results file that already exists, or a queue that does not, must fail
        // without declaring DPI awareness, enumerating windows or opening a capture
        // resource -- and above all without appending to a file that was some earlier
        // session's evidence.
        UF_TRY_VALUE(paths, validateDriveIpcPaths(args));

        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

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

        // Built before the target, for the same reason `run` orders it that way:
        // a model directory that will not build an engine must fail before any
        // window is enumerated.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        UF_TRY_VALUE(bound, platform::bindTarget(args.selector));

        // Every bound below is the same field with the same default `run` passes, and
        // startOperatorSession is where the generation's front-end claim is latched: a
        // generation a task run already drove refuses here.
        UF_TRY_VALUE(
            session,
            host.startOperatorSession(
                generation,
                task::TaskRunConfig{
                    .frameSource             = std::move(bound.frameSource),
                    .actionSink              = std::move(bound.actionSink),
                    .ocrEngine               = std::move(ocrEngine),
                    .liveFingerprint         = bound.liveFingerprint,
                    .maximumPixelComparisons = args.budget,
                    .recognitionTimeout      = args.recognitionTimeout,
                    .maxActionFrameAge       = args.maxFrameAge,
                    .tracePath               = args.trace,
                }
            )
        );

        return driveSession(*session, args, paths, cancellation.token());
    }
}
