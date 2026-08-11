#include "explore.hpp"

#include "platform/ocr-engine-binding.hpp"
#include "platform/target-binding.hpp"
#include "platform/windows-console-cancellation.hpp"
#include "project-skeleton.hpp"

#include <controller/discovery.hpp>
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
        // reading it could repeat an authoring write the agent sent only once.
        UF_TRY_VALUE(paths, validateExploreIpcPaths(args));

        UF_TRY_VALUE(cancellation, platform::ConsoleCancellation::install());

        // Before the project is loaded, because an authoring session's first write
        // would otherwise find the directory missing and the store it writes through
        // will not create one (see project-skeleton.hpp).
        UF_TRY(ensureProjectSkeleton(args.project));

        auto host = task::TaskHost{};
        UF_TRY_VALUE(
            generation,
            host.openAnnotationProject(
                args.project,
                task::TaskHostConfig{
                    .externalCancellation = cancellation.token(),
                }
            )
        );

        // A model directory that will not build an engine must fail before any
        // window is enumerated.
        UF_TRY_VALUE(ocrEngine, platform::bindOcrEngine(args.ocrModels));

        UF_TRY_VALUE(bound, platform::bindTarget(WindowHandle{args.windowHandle}));

        // Every bound below is the same field with the same default the other
        // front-ends pass. startExplorationSession latches the Annotation claim,
        // so a Runtime generation or an already claimed generation refuses here.
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

    auto exploreCancellationRequested() noexcept -> bool
    {
        return platform::ConsoleCancellation::stopRequested();
    }
}
