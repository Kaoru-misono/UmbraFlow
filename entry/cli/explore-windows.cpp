#include "explore.hpp"

#include "platform/ocr-engine-binding.hpp"
#include "platform/target-binding.hpp"
#include "platform/windows-console-cancellation.hpp"
#include "project-skeleton.hpp"

#include <core/error/result.hpp>

#include <task/exploration-session.hpp>
#include <task/task-host.hpp>

#include <utility>

namespace uf::cli
{
    auto exploreProduct(ExploreArgs const& args) -> Result<task::TaskRunReport>
    {
        // First, before anything observable exists: the cursor decides whether chunks
        // already in the queue have run, so a session that bound a target before
        // reading it could re-deliver clicks the agent never asked for twice.
        UF_TRY_VALUE(paths, validateExploreIpcPaths(args));

        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // Before the project is loaded, because an authoring session's first write
        // would otherwise find the directory missing and the store it writes through
        // will not create one (see project-skeleton.hpp). Only `explore` does this:
        // check, run and drive all READ a project somebody already authored, so a
        // missing directory there is evidence about that project.
        UF_TRY(ensureProjectSkeleton(args.project));

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

        // Before the target, as `run` and `drive` order it: a model directory that
        // will not build an engine must fail before any window is enumerated.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        UF_TRY_VALUE(bound, platform::bindTarget(args.selector));

        // Every bound below is the same field with the same default the other two
        // front-ends pass. startExplorationSession latches the generation's
        // front-end claim, so a generation a task run or an operator already drove
        // refuses here.
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
