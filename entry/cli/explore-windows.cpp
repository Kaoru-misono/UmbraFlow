#include "explore.hpp"

#include "platform/ocr-engine-binding.hpp"
#include "platform/target-binding.hpp"
#include "platform/windows-console-cancellation.hpp"

#include <core/error/result.hpp>

#include <task/exploration-session.hpp>
#include <task/task-host.hpp>

#include <utility>

namespace uf::cli
{
    auto exploreProduct(ExploreArgs const& args) -> Result<task::TaskRunReport>
    {
        // The IPC paths and the cursor are checked first, before anything
        // observable exists. It matters more here than for `drive`: the cursor is
        // what decides whether chunks already in the queue have run, and a session
        // that bound a target before reading it could deliver clicks the agent
        // never asked for a second time.
        UF_TRY_VALUE(paths, validateExploreIpcPaths(args));

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

        // Built before the target, for the reason `run` and `drive` order it that
        // way: a model directory that will not build an engine must fail before
        // any window is enumerated.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        UF_TRY_VALUE(bound, platform::bindTarget(args.selector));

        // Every bound below is the same field with the same default the other two
        // front-ends pass, and startExplorationSession is where the generation's
        // front-end claim is latched: a generation a task run or an operator
        // already drove refuses here.
        UF_TRY_VALUE(
            session,
            host.startExplorationSession(
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

        return exploreSession(*session, args, paths, cancellation.token());
    }
}
